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

// Runs /sbin/init, prints its exit code, and shuts down.
//   Q: When exactly does the kernel shut down?
//   Q: Why is it weak, and what can replace it?

#include "boot/kernel_main.h"
#include "fs/ext2.h"
#include "io/ramdisk.h"
#include "lib/debug.h"
#include "lib/print.h"
#include "mm/vmm.h"
#include "proc/elf.h"
#include "proc/pcb.h"
#include "proc/tcb.h"
#include "sched/async.h"

namespace {

constexpr uint64_t INIT_STACK_SIZE = 64 * 1024;

// Loads program into process and sets thread to start there.
//   Q: Where does the thread's stack come from?
//   Q: Which registers does it set?
sync<bool> exec_phase2(Thread thread, StrongRef<Node> program,
                       const char *name) {
  const uint64_t entry = co_await ELF::load(thread->process, program);
  if (entry == 0) {
    KPRINT("? isn't a program the loader takes\n", name);
    co_return false;
  }

  // Its stack: pages fault in on demand.
  const std::expected<void *, int64_t> stack =
      co_await VMM::mmap(thread->process, nullptr, INIT_STACK_SIZE,
                         VMM::PROT_READ | VMM::PROT_WRITE,
                         VMM::MAP_PRIVATE | VMM::MAP_ANONYMOUS, {}, 0);
  if (!stack) {
    co_return false;
  }

  // where it starts in user mode
  thread->state.rip = entry;
  uint64_t top = uint64_t(*stack) + INIT_STACK_SIZE;
  static const char init_name[] = "/sbin/init";
  top -= sizeof(init_name);
  if (!co_await VMM::copy_out(thread->process, VA{top}, init_name, sizeof(init_name))) co_return false;
  uint64_t words[] = {1, top, 0, 0, 0, 0};
  top = (top - sizeof(words)) & ~uint64_t(15);
  if (!co_await VMM::copy_out(thread->process, VA{top}, words, sizeof(words))) co_return false;
  thread->state.rsp = top;
  co_return true;
}

} // namespace

// Q: why are they global variables instead of local to kernel_main?
//       there are at least two reasons.
static StrongRef<BlockIO> disk{};
static StrongRef<Ext2> fs{};

async<void> kernel_main() {
  // (0) find the disk
  disk = StrongRef<BlockIO>{new RamDisk(0)};

  // (1) mount the file system on it
  fs = co_await Ext2::mount(disk);

  // (2) find /sbin/init
  auto init = co_await fs->find(fs->root, "/sbin/init");
  if (init == nullptr) {
    KPANIC("? not found\n", "/sbin/init");
  }

  // The thread lives only in the block below.
  //   Q: Why would waiting for exited while holding the thread never end?
  async<int> exited{};
  {
    // (3) a new process, and its first thread
    const Thread thread = Process::create();
    ASSERT(thread != nullptr);         // the first one, there are pids left
    ASSERT(thread->process->pid == 1); // init is always pid 1
    thread->process->fs = fs;

    // (4) load /sbin/init into it
    if (!co_await exec_phase2(thread, init, "/sbin/init")) {
      KPANIC("couldn't start ?\n", "/sbin/init");
    }

    // (5) run it
    exited = thread->process->exited;
    thread.schedule();
  }

  // (6) wait for it to exit: its status goes to the debug console
  const int status = co_await exited;
  KPRINT("init exited: ?\n", Dec(wait_status::shell_code(status)));
  shutdown(false);
}
