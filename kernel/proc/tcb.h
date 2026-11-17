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

#include "arch/saved_state.h"
#include "lib/debug.h"
#include "lib/shared.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "sched/atomic.h"
#include <cstddef>
#include <cstdint>
#include <utility>

// The thread control block, reached only through a Thread.
//   Q: Who can hold a reference to it, and for how long?
//   Q: Where is a live thread's Thread held at each point in its life?
struct TCB {
  // Has it ended? Set once, read by any core.
  Atomic<bool> ended_{false};
  // Its ready-queue event while it's runnable.
  ResumeThread resume_event{};
  // Its process, kept alive by the thread.
  //   Q: When is the process's memory freed?
  const Process process;

  // Its user-mode state, saved on every way into the kernel.
  //   Q: Which ways into the kernel save it, and where's the code?
  SavedState state{};

  // Its thread ID; the process's pid is its first thread's.
  const int tid;

  // Is it the process's first thread (Linux's thread group leader)?
  //   Q: Whose exit code becomes the process's status, and when?
  const bool is_leader;

  // System calls in a row that went straight back to user mode.
  //   Q: Why make the thread take a turn in the ready queue after a few?
  uint32_t direct_returns = 0;
  uint64_t blocked = 0;
  bool signal_checked = false;
  int fault_signal = 0;
  int fault_code = 1;
  uint64_t fault_address = 0;
  struct SignalFrame {
    SavedState saved;
    uint64_t blocked, user_context;
    SignalFrame *next;
  };
  SignalFrame *signal_frames = nullptr;

  // CLONE_CHILD_CLEARTID: where to write 0 when it ends.
  //   Q: Why does the kernel write it through copy_out?
  //   Q: Who waits on that address?
  int *clear_child_tid = nullptr;

  TCB(const TCB &) = delete;
  TCB &operator=(const TCB &) = delete;

  // Only the last Thread deletes it, once the thread has ended.
  ~TCB() {
    // A live thread is always held where it is (a core, the ready queue, a
    // coroutine working for it): losing every Thread before it ended is a
    // bug.
    ASSERT(ended());
    while (signal_frames) {
      auto f = signal_frames;
      signal_frames = f->next;
      delete f;
    }
  }

  // Has the thread ended? Its TCB can still be around.
  bool ended() const { return ended_; }

private:
  // Thread::create makes it.
  TCB(Process process, int tid, bool is_leader)
      : process(std::move(process)), tid(tid), is_leader(is_leader) {}
  friend class StrongRef<TCB>;
};
