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

#include "syscall/syscall.h"

#include "arch/gdt.h"
#include "arch/machine.h"
#include "arch/x86_64.h"
#include "proc/ccb.h"
#include "proc/pcb.h"
#include "proc/signals.h"
#include "proc/tcb.h"
#include <utility>

// the entry point, in machine.S
extern "C" void syscallEntry_();

namespace {

// sysret's selector base: SS = base + 8, CS = base + 16.
//   Q: Why must the user data segment come right before user code?
constexpr uint64_t SYSRET_BASE = (GDT::USER_DS & ~3) - 8;
static_assert((SYSRET_BASE + 8) == (GDT::USER_DS & ~3));
static_assert((SYSRET_BASE + 16) == (GDT::USER_CS & ~3));
// syscall loads CS from STAR[47:32] and SS = CS + 8
static_assert(GDT::KERNEL_DS == GDT::KERNEL_CS + 8);

// RFLAGS bits cleared on entry: TF, IF, DF, AC.
//   Q: Why must IF and DF be clear when the kernel starts?
constexpr uint64_t ENTRY_FLAGS_MASK = 0x100 | 0x200 | 0x400 | 0x40000;

// After this many direct returns, the thread takes its turn anyway.
//   Q: Without preemption, why is this needed?
constexpr uint32_t DIRECT_RETURNS = 64;

// A system call this kernel doesn't have.
sync<int64_t> no_such_call() { co_return Syscall::ENOSYS; }

} // namespace

void Syscall::init_core() {
  Msr::IA32_LSTAR().write(uint64_t(syscallEntry_));
  Msr::IA32_STAR().write((SYSRET_BASE << 48) |
                         (uint64_t(GDT::KERNEL_CS) << 32));
  Msr::I32_FMASK().write(ENTRY_FLAGS_MASK);
}

// Runs the thread's system call.
//   Q: Why can't the thread's process go away during the call?
//   Q: Which part of the thread's state changes?
sync<int64_t> Syscall::dispatch(Thread thread, uint32_t number) {
  switch (number) {
  case 13:
  case 15:
  case 35:
  case 37:
  case 62:
  case 79:
  case 80:
  case 202:
  case 292:
  case 293:
    return extended(std::move(thread), number);
  case 2:
  case 3:
  case 8:
  case 257:
    return filesystem(std::move(thread), number);
  case 57:
    return fork_process(std::move(thread));
  case 61:
    return wait_process(std::move(thread));
  case 59:
  case 322:
    return exec_process(std::move(thread), number == 322);
  case READ:
    return read(std::move(thread));
  case WRITE:
    return write(std::move(thread));
  case MMAP:
    return mmap(std::move(thread));
  case MUNMAP:
    return munmap(std::move(thread));
  case CLONE3:
    return clone3(std::move(thread));
  default:
    return no_such_call();
  }
}

sync<void> Syscall::finish(Thread thread, sync<int64_t> call) {
  thread->state.rax = uint64_t(co_await call);
  thread->direct_returns = 0;
  std::move(thread).schedule();
}

// Starts the system call; true if the thread can go straight back.
//   Q: What are the three ways it can go on when it returns false?
//   Q: Why must this be its own noinline function?
//   Q: How does sync's peek decide between the two paths?
[[gnu::noinline]] bool Syscall::start() {
  const Thread thread = Thread::current();
  const Process process = thread->process;
  SavedState &state = thread->state;
  // Like Linux: the number is EAX, RAX's high half doesn't count.
  //   Q: Why is -1 still not a system call?
  const uint32_t number = uint32_t(state.rax);
  if (process->exiting()) {
    // exit_group from another thread: this one ends instead
    thread.exit(0);
    return false;
  }
  if (number == EXIT) {
    if (process->pid == 1 && thread->is_leader)
      shutdown(false);
    thread.exit(int64_t(state.rdi));
    return false;
  }
  if (number == EXIT_GROUP) {
    if (process->pid == 1)
      shutdown(false);
    process.exit_group(wait_status::exited(int64_t(state.rdi)));
    thread.exit(int64_t(state.rdi));
    return false;
  }

  if (number == SCHED_YIELD) {
    // its whole point: the others in the ready queue go first
    state.rax = 0;
    thread->direct_returns = 0;
    thread.schedule();
    return false;
  }

  if (number == 169)
    shutdown(false);
  if (number == 39 || number == 110 || number == 186) {
    state.rax =
        number == 39 ? process->pid
        : number == 110
            ? (process->ppid && process->parent_exited.peek() ? 1
                                                              : process->ppid)
            : thread->tid;
    if (Signals::pending(thread)) {
      thread.schedule();
      return false;
    }
    return true;
  }
  sync<int64_t> call = dispatch(thread, number);
  const auto result = call.peek();
  if (!result) {
    // it waits: it goes on by itself, and makes the thread runnable
    (void)finish(thread, std::move(call));
    return false;
  }
  state.rax = uint64_t(*result);

  if (++thread->direct_returns >= DIRECT_RETURNS || process->exiting()) {
    // Its turn in the ready queue (it ends instead if exiting).
    thread->direct_returns = 0;
    thread.schedule();
    return false;
  }
  if (Signals::pending(thread)) {
    thread.schedule();
    return false;
  }
  return true;
}

// The system call's C++ entry, from syscallEntry_.
//   Q: Which stack does it run on, and what's on it?
//   Q: When does it not return, and how does the thread go on then?
extern "C" const SavedState *syscallEntry() {
  sti();
  if (!Syscall::start()) {
    CCB::restart_event_loop();
  }
  return Thread::prepare_return();
}
