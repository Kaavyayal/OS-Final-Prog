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

#include "proc/process.h"

#include "arch/machine.h"
#include "lib/debug.h"
#include "mm/vmm.h"
#include "proc/pcb.h"
#include "proc/signals.h"
#include "proc/tcb.h"
#include "proc/thread.h"
#include "sched/event_loop.h"

// The kernel's page tables (Limine's), read at boot.
//   Q: Which core's CR3 is this, and when is it read?
static const uint64_t kernel_cr3 = get_cr3();

const Process Process::the_kernel{
    StrongRef<PCB>::make(kernel_cr3, uint64_t(0), VMM::new_vmes())};

Process Process::kernel() { return the_kernel; }

Process::Process(const StrongRef<PCB> &pcb) : pcb_(pcb) {}

Thread Process::create() {
  const int pid = Thread::new_tid();
  if (pid < 0) {
    return {};
  }
  const Process process{StrongRef<PCB>::make(VMM::new_page_tables(),
                                             uint64_t(pid), VMM::new_vmes())};
  Signals::register_process(process);
  // Outstanding work until it's gone; the event loop waits for it.
  impl::started.fetch_add(1);
  // its first thread: its tid is the pid, and it's the leader
  return Thread::create(process, pid, true);
}

void Process::exit_group(int status) const {
  {
    LockGuard g{pcb_->lock};
    if (!pcb_->exiting_) {
      pcb_->group_status = status;
      pcb_->exiting_.set(true);
    }
  }
  Futex::cancel(*this);
}

void Process::thread_ended(Thread thread, int64_t code) const {
  // only the leader's code counts
  if (thread->is_leader) {
    LockGuard g{pcb_->lock};
    pcb_->leader_status = wait_status::exited(code);
  }
}
