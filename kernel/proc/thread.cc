/* Copyright (C) 2026 Ahmed Gheith and contributors.
 *
 * Use restricted to classroom projects.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "proc/thread.h"

#include "arch/gdt.h"
#include "arch/machine.h"
#include "lib/debug.h"
#include "mm/vmm.h"
#include "proc/ccb.h"
#include "proc/pcb.h"
#include "proc/signals.h"
#include "proc/tcb.h"
#include "sched/atomic.h"
#include "sched/event_loop.h"
#include <utility>

namespace {

// tids and pids come from the same counter, like on Linux.
//   Q: Why can it never wrap around?
Atomic<uint64_t> next_tid{1};
// The largest tid: tids are ints (Linux's pid_t).
constexpr uint64_t MAX_TID = 0x7FFFFFFF;

// The RFLAGS bits user code may change.
//   Q: Why can't user code set IF or IOPL?
constexpr uint64_t USER_FLAGS = 0x40CD5;
// RFLAGS bits always set: IF, and bit 1.
constexpr uint64_t FORCED_FLAGS = 0x202;

// restore_context_ (machine.S) has the user selectors built in.
static_assert(GDT::USER_CS == 0x43);
static_assert(GDT::USER_DS == 0x3b);

// Is va a low-mem (user) address?
bool is_user(uint64_t va) { return va < (UINT64_C(1) << 47); }

// The rest of Thread::exit: clears clear_child_tid, then tells the process.
//   Q: Why does it run as its own coroutine?
//   Q: Why must thread hold on to the TCB until thread_ended?
async<void> thread_exit(Thread thread, int *clear_child_tid, int64_t code) {
  const Process process = thread->process;
  if (clear_child_tid != nullptr) {
    // A bad address is the thread's problem, it's gone anyway
    const int zero = 0;
    co_await VMM::copy_out(process, VA{uint64_t(clear_child_tid)}, &zero,
                           sizeof(zero));
  }
  if (clear_child_tid)
    Futex::wake(process, uint64_t(clear_child_tid), 1);
  process.thread_ended(std::move(thread), code);
}
} // namespace

////////////////////////////// Thread //////////////////////////////

Thread::Thread(const StrongRef<TCB> &tcb) : tcb_(tcb) {}

Thread Thread::the_kernel{};

Thread Thread::kernel() { return the_kernel; }

const Thread &Thread::current() { return CCB::current()->running.thread(); }

void Thread::init_system() {
  ASSERT(the_kernel == nullptr);
  // not one of Process::kernel()'s threads it counts: that process never ends
  the_kernel = create(Process::kernel(), 0, false);
}

void Thread::enter() const {
  CCB *ccb = CCB::current();
  if (ccb->running.thread() == *this) {
    if (get_cr3() != tcb_->process->cr3)
      set_cr3(tcb_->process->cr3);
    return;
  }
  ccb->running.set(*this);
}

Thread Thread::create(Process process, int tid, bool is_leader) {
  return Thread{StrongRef<TCB>::make(std::move(process), tid, is_leader)};
}

int Thread::new_tid() {
  const uint64_t tid = next_tid.fetch_add(1);
  return tid <= MAX_TID ? int(tid) : -1;
}

Thread Thread::clone() const {
  const int tid = new_tid();
  if (tid < 0) {
    return {};
  }
  const Thread child = create(tcb_->process, tid, false);
  child->state.copy_from(tcb_->state);
  child->blocked = tcb_->blocked;
  return child;
}

void Thread::exit(this Thread self, int64_t code) {
  ASSERT(kernel() != self);
  int *const clear_child_tid = self->clear_child_tid;
  self->ended_.set(true);
  // Last: thread_exit holds the thread from here on
  thread_exit(std::move(self), clear_child_tid, code);
}

void Thread::schedule(this Thread self, bool preempted) {
  TCB *const tcb = self.tcb_.operator->(); // before self is moved from
  // the queued event holds the thread, until dispatch
  tcb->resume_event.thread = std::move(self);
  if (preempted)
    impl::preempted_queue.add(&tcb->resume_event);
  else
    impl::ready_queue.add(&tcb->resume_event);
}

namespace {
sync<void> signal_resume(Thread t, sync<void> delivery) {
  co_await delivery;
  if (!t->ended()) {
    t->signal_checked = true;
    t.schedule();
  }
}
} // namespace
void ResumeThread::dispatch() {
  // From here on the core holds the thread, not the event.
  thread.enter();
  thread = nullptr;
  // Going back to user mode abandons this stack: a Thread only in here.
  //   Q: Why can nothing with a destructor be live when restore_context_ runs?
  {
    const Thread current = Thread::current();
    if (current->process->exiting()) {
      // exit_group: it ends instead
      current.exit(0);
      return;
    }
  }
  {
    const Thread t = Thread::current();
    if (!t->signal_checked && Signals::pending(t)) {
      auto delivery = Signals::deliver(t);
      if (!delivery.await_ready()) {
        (void)signal_resume(t, std::move(delivery));
        return;
      }
      if (t->ended())
        return;
    }
    t->signal_checked = false;
  }
  restore_context_(Thread::prepare_return());
}

SavedState *Thread::prepare_return() {
  // No interrupts until iretq or sysret: GS is about to be the user's.
  asm volatile("cli" ::: "memory");

  // The core's thread, which the core keeps until it comes back.
  SavedState *const state = CCB::current()->running.saved_state();
  // A non-canonical RIP would fault in kernel mode with the user's GS.
  ASSERT(is_user(state->rip));
  state->rflags = (state->rflags & USER_FLAGS) | FORCED_FLAGS;

  // The thread's page tables are already loaded (CCB::Running::set).
  const uint64_t cr3 = CCB::current()->running.thread()->process->cr3;
  if (get_cr3() != cr3)
    set_cr3(cr3);
  return state;
}
