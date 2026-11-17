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

#include "arch/machine.h"
#include "lib/debug.h"
#include "lib/shared.h"
#include "mm/vmm.h"
#include "proc/ccb.h"
#include "proc/files.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "sched/async.h"
#include "sched/atomic.h"
#include "sched/event_loop.h"
#include "sched/mutex.h"
#include "sched/spin_lock.h"
#include "sched/sync.h"
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Linux wait statuses: how to make one, and what a shell shows.
//   Q: How do the W* macros in user code decode them?
namespace wait_status {

// The status for exit(code): code's low 8 bits.
constexpr int exited(int64_t code) { return int((code & 0xff) << 8); }

// The status for death by signal (no core dump).
constexpr int killed(int signal) { return signal & 0x7f; }
constexpr int SIGSEGV = 11;

// $?: the exit code, or 128 + the signal that killed it.
constexpr int shell_code(int status) {
  return (status & 0x7f) == 0 ? (status >> 8) & 0xff : 128 + (status & 0x7f);
}

} // namespace wait_status

// Processes and threads: what runs, what owns memory, when it ends.
//   Q: When does a process end, and whose code is its status?
//   Q: How do other threads stop after exit_group?
//   Q: Which address space is loaded on a core, and when does it change?
//   Q: Why doesn't kernel code depend on the loaded address space?
// The process control block, reached only through a Process.
//   Q: Why can't code hold a PCB * of its own?
struct PCB {
  const uint64_t pid; // its first thread's tid, 0 for kernel()

  // The value for CR3: its PML4's physical address.
  const uint64_t cr3;
  // Its VMEs: sorted, non-overlapping, starting with a page-0 dummy.
  Mutex<impl::VME *> vmes;
  StrongRef<Ext2> fs;
  Mutex<StrongRef<Node>> cwd{};
  struct Action {
    uint64_t handler = 0, flags = 0, restorer = 0, mask = 0;
  };
  SpinLock signal_lock;
  Action actions[16];
  uint64_t pending = 0, alarm_at = 0;
  uint64_t senders[16]{};
  uint64_t ppid = 0;
  async<int> parent_exited;
  Mutex<FileTable> files{};
  Mutex<ChildProcess *> children{nullptr};

private:
  // Sets exited, once; only ~PCB uses it.
  //   Q: Why must it be declared before exited?
  async_setter<int> exit_setter{};

public:
  // The process's end: its status, once its memory is gone.
  //   Q: Why must you not wait for it while holding the process?
  const async<int> exited = exit_setter.result();

  PCB(const PCB &) = delete;
  PCB &operator=(const PCB &) = delete;

  // Has exit_group been called?
  bool exiting() const { return exiting_; }

private:
  // Protects its life: exiting_ and the statuses.
  SpinLock lock{};
  Atomic<bool> exiting_{false}; // written under lock, read anywhere
  // Exit statuses (see wait_status)
  int group_status = 0;  // exit_group's, if exiting_
  int leader_status = 0; // its first thread's exit, once it ended

  // Process does the work; only StrongRef::make builds a PCB.
  friend class Process;
  friend class StrongRef<PCB>;
  friend class impl::Middle<PCB>;
  PCB(uint64_t cr3, uint64_t pid, impl::VME *vmes);

  // The process is over: frees its memory, then sets exited.
  //   Q: Who holds the last reference, usually?
  //   Q: Why is no lock or TLB shootdown needed here?
  ~PCB();
};
