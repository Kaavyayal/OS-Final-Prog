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

#include "lib/errno.h"
#include "proc/thread.h"
#include "sched/sync.h"
#include <cstdint>

struct SavedState;

// Where a system call comes in, called by syscallEntry_.
extern "C" const SavedState *syscallEntry();

// System calls: Linux's x86-64 conventions, one coroutine per call.
//   Q: Which registers hold the number, arguments and result?
//   Q: Why should a call avoid waiting on its common path?
//   Q: What happens to the core when a call has to wait?
class Syscall {
public:
  static constexpr uint64_t READ = 0;    // read(fd, buf, len), stdin only
  static constexpr uint64_t WRITE = 1;   // write(fd, buf, len)
  static constexpr uint64_t MMAP = 9;    // mmap(addr, length, prot, flags, fd,
                                         //      offset), anonymous only
  static constexpr uint64_t MUNMAP = 11; // munmap(addr, length)
  static constexpr uint64_t SCHED_YIELD = 24; // sched_yield(), always blocks
  static constexpr uint64_t EXIT = 60;        // exit(code), the calling thread
  static constexpr uint64_t EXIT_GROUP =
      231;                                // exit_group(code), the whole process
  static constexpr uint64_t CLONE3 = 435; // clone3(args, size), threads only

  // clone3's flags (Linux's values).
  //   Q: Which are required for a thread, and which are ignored?
  static constexpr uint64_t CLONE_VM = 0x100;
  static constexpr uint64_t CLONE_FS = 0x200;
  static constexpr uint64_t CLONE_FILES = 0x400;
  static constexpr uint64_t CLONE_SIGHAND = 0x800;
  static constexpr uint64_t CLONE_THREAD = 0x10000;
  static constexpr uint64_t CLONE_SYSVSEM = 0x40000;
  static constexpr uint64_t CLONE_SETTLS = 0x80000;
  static constexpr uint64_t CLONE_PARENT_SETTID = 0x100000;
  static constexpr uint64_t CLONE_CHILD_CLEARTID = 0x200000;

  // Error results: Linux's -errno values (lib/errno.h).
  static constexpr int64_t EPERM = Errno::EPERM;
  static constexpr int64_t E2BIG = Errno::E2BIG;
  static constexpr int64_t EBADF = Errno::EBADF;
  static constexpr int64_t EAGAIN = Errno::EAGAIN;
  static constexpr int64_t EFAULT = Errno::EFAULT;
  static constexpr int64_t EINVAL = Errno::EINVAL;
  static constexpr int64_t ENOSYS = Errno::ENOSYS;

  // Sets up the syscall instruction on this core.
  //   Q: Which MSRs does it write, and what goes in each?
  static void init_core();

  // The end of low-mem, user memory's canonical half.
  static constexpr uint64_t USER_END = UINT64_C(1) << 47;

  // Is [p, p + n) in low-mem?
  //   Q: Why compare n with USER_END - p rather than p + n with USER_END?
  static bool user_range(uint64_t p, uint64_t n) {
    return p < USER_END && n <= USER_END - p;
  }

private:
  // Runs the calling thread's system call, number (EAX).
  //   Q: Where does it find the arguments?
  static sync<int64_t> dispatch(Thread thread, uint32_t number);

  // The system calls, one file each (<name>.cc).
  static sync<int64_t> extended(Thread thread, uint32_t number);
  static sync<int64_t> read(Thread thread);
  static sync<int64_t> write(Thread thread);
  static sync<int64_t> mmap(Thread thread);
  static sync<int64_t> munmap(Thread thread);
  static sync<int64_t> clone3(Thread thread);
  static sync<int64_t> filesystem(Thread thread, uint32_t number);
  static sync<int64_t> fork_process(Thread thread);
  static sync<int64_t> wait_process(Thread thread);
  static sync<int64_t> exec_process(Thread thread, bool at);

  // Finishes a system call that had to wait.
  //   Q: How does the thread get back to user mode afterwards?
  static sync<void> finish(Thread thread, sync<int64_t> call);

  // Starts the system call; true if it's already done.
  static bool start();
  friend const SavedState * ::syscallEntry();
};
