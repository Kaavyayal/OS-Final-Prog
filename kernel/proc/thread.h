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

#pragma once

// The Thread handle only; include tcb.h to use its fields.

#include "lib/shared.h"
#include "sched/event.h"
#include <cstddef>
#include <cstdint>

struct TCB;
class Process;

// A thread: a reference-counted handle to its TCB.
//   Q: Which thread does a core run as, and when does that change?
//   Q: Why must a thread's coroutines finish before it returns to user mode?
//   Q: Who holds a Thread, and when is the TCB deleted?
class Thread {
public:
  // No thread.
  Thread() {}
  Thread(std::nullptr_t) {}
  Thread &operator=(std::nullptr_t) {
    tcb_ = nullptr;
    return *this;
  }

  // Its fields
  TCB *operator->() const { return tcb_.operator->(); }

  // The same thread? Only ==: C++20 derives !=.
  bool operator==(const Thread &other) const { return tcb_ == other.tcb_; }
  bool operator==(std::nullptr_t) const { return tcb_ == nullptr; }

  // The thread this core runs as.
  //   Q: Why copy it rather than keep the reference?
  //   Q: Why must code that abandons its stack drop its copies first?
  static const Thread &current();

  // The kernel thread: what a core runs as with no user thread.
  //   Q: Why does it never run in user mode or end?
  static Thread kernel();

  // Creates the kernel thread, once, before any core runs.
  static void init_system();

  // Makes this core run as the thread.
  void enter() const;

  // Queues the thread to return to user mode, handing it to the queued event.
  //   Q: What happens if its process is exiting by the time it runs?
  //   Q: Why does it take the thread by value, and when should callers move?
  void schedule(this Thread self, bool preempted = false);

  // Prepares any return to user mode and returns the saved state: iretq
  // (restore_context_) or sysret (syscallEntry_) takes it from there.
  //   Q: Why must interrupts be off from here on?
  //   Q: Which RFLAGS bits does it fix, and why?
  //   Q: When does the kernel return with sysret instead of iretq?
  static SavedState *prepare_return();

  // Ends the thread, and the process if it was the last.
  //   Q: Why is clearing clear_child_tid asynchronous?
  //   Q: When does the TCB itself go away?
  void exit(this Thread self, int64_t code);

  // A new thread in this process with a copy of this one's state.
  //   Q: What does it return when tids run out?
  Thread clone() const;

private:
  StrongRef<TCB> tcb_;

  // Wraps the first StrongRef to a new TCB.
  explicit Thread(const StrongRef<TCB> &tcb);

  // The kernel thread, set by init_system, never goes away.
  static Thread the_kernel;

  friend class Process;
  // A new tid, or -1 once they ran out.
  //   Q: Why are tids never reused?
  static int new_tid();
  // A new thread in process with default state, and its only Thread.
  //   Q: What must be set before it runs?
  static Thread create(Process process, int tid, bool is_leader);
};

// A thread's event: resumes it in user mode.
//   Q: What keeps the thread alive while it's queued?
struct ResumeThread final : Event {
  Thread thread{};
  void dispatch() override;
};

// A handle is just the StrongRef: machine.S knows CCB's layout.
static_assert(sizeof(Thread) == sizeof(StrongRef<TCB>));
