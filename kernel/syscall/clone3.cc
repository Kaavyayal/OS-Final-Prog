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

#include "mm/physmem.h"
#include "mm/vmm.h"
#include "proc/process.h"
#include "proc/tcb.h"
#include <optional>
#include <utility>

namespace {

// struct clone_args, as far as Linux has it (CLONE_ARGS_SIZE_VER2).
//   Q: How do older callers (a shorter struct) and newer ones (longer) work?
struct CloneArgs {
  uint64_t flags;
  uint64_t pidfd;
  uint64_t child_tid;
  uint64_t parent_tid;
  uint64_t exit_signal;
  uint64_t stack;
  uint64_t stack_size;
  uint64_t tls;
  uint64_t set_tid;      // pids for the child, in each pid namespace
  uint64_t set_tid_size; // how many
  uint64_t cgroup;       // with CLONE_INTO_CGROUP
};
static_assert(sizeof(CloneArgs) == 88);

// The shortest struct a caller can pass (CLONE_ARGS_SIZE_VER0: up to tls).
constexpr uint64_t SIZE_VER0 = 64;

// Linux's limit on set_tid_size (MAX_PID_NS_LEVEL).
constexpr uint64_t MAX_PID_NS_LEVEL = 32;

// Linux's largest pid_max (PID_MAX_LIMIT): set_tid's pids must be below it.
constexpr int PID_MAX = 4 * 1024 * 1024;

// Linux's first checks, before it reads set_tid: 0 if fine, or EINVAL.
int64_t check_struct(const CloneArgs &args) {
  if (args.set_tid_size > MAX_PID_NS_LEVEL ||
      (args.set_tid == 0) != (args.set_tid_size == 0)) {
    return Syscall::EINVAL;
  }
  // exit_signal must be a signal (0..64), and for a thread it must be 0
  if (args.exit_signal > 64) {
    return Syscall::EINVAL;
  }
  return 0;
}

// The rest, in Linux's order: 0 if fine, or the error. first_tid is
// set_tid's first pid, if it has one.
//   Q: Which flags must be set, and which are accepted but ignored?
//   Q: Why is a stack of 0 bytes at address 0 fine?
int64_t check(const CloneArgs &args, int first_tid) {
  // This kernel makes threads only: Linux takes more (a fork, vfork, ...).
  constexpr uint64_t REQUIRED =
      Syscall::CLONE_VM | Syscall::CLONE_SIGHAND | Syscall::CLONE_THREAD;
  constexpr uint64_t ALLOWED =
      REQUIRED | Syscall::CLONE_FS | Syscall::CLONE_FILES |
      Syscall::CLONE_SYSVSEM | Syscall::CLONE_SETTLS |
      Syscall::CLONE_PARENT_SETTID | Syscall::CLONE_CHILD_CLEARTID;
  if ((args.flags & REQUIRED) != REQUIRED || (args.flags & ~ALLOWED) != 0) {
    return Syscall::EINVAL;
  }
  if (args.exit_signal != 0) {
    return Syscall::EINVAL;
  }
  // No stack (the child starts on the parent's), or a range of user memory.
  if ((args.stack == 0) != (args.stack_size == 0)) {
    return Syscall::EINVAL;
  }
  if (args.stack != 0 && !Syscall::user_range(args.stack, args.stack_size)) {
    return Syscall::EINVAL;
  }
  // Like Linux (ARCH_SET_FS): a non-canonical FS base would make wrfsbase
  // fault in the kernel.
  if ((args.flags & Syscall::CLONE_SETTLS) != 0 && args.tls >= VA_END.va()) {
    return Syscall::EPERM;
  }
  // Last, set_tid: one pid namespace, so one pid, and picking it needs
  // privileges this process doesn't have.
  if (args.set_tid_size != 0) {
    if (args.set_tid_size > 1 || first_tid < 1 || first_tid >= PID_MAX) {
      return Syscall::EINVAL;
    }
    return Syscall::EPERM;
  }
  return 0;
}

// Makes the child, a copy of parent that doesn't run yet.
//   Q: Which registers differ from the parent's?
//   Q: What does it return when tids run out?
Thread make_child(Thread parent, const CloneArgs &args) {
  const Thread child = parent.clone();
  if (child == nullptr) {
    return child;
  }
  child->state.rax = 0;
  if (args.stack != 0) {
    child->state.rsp = args.stack + args.stack_size;
  }
  if ((args.flags & Syscall::CLONE_SETTLS) != 0) {
    child->state.fs_base = args.tls;
  }
  if ((args.flags & Syscall::CLONE_CHILD_CLEARTID) != 0) {
    child->clear_child_tid = (int *)args.child_tid;
  }
  return child;
}

// Are the size bytes at va all zero? Nothing if they aren't all readable.
//   Q: Why must the part of the struct Linux doesn't know be zero?
sync<std::optional<bool>> all_zero(Process process, VA va, uint64_t size) {
  bool zero = true;
  const bool read = co_await VMM::with_user_pages(
      std::move(process), va, size, false,
      [&zero](char *bytes, uint64_t, uint64_t count) {
        for (uint64_t i = 0; i < count; i++) {
          zero = zero && bytes[i] == 0;
        }
        return count;
      });
  if (!read) {
    co_return std::nullopt;
  }
  co_return zero;
}

} // namespace

// clone3, threads only: a new thread in the caller's process.
//   Q: Why tell the parent the tid before the child can run?
//   Q: When does it have to wait, and for what?
//   Q: Why does the child run even if CLONE_PARENT_SETTID fails?
sync<int64_t> Syscall::clone3(Thread thread) {
  const VA uargs = thread->state.arg<0, VA>();
  const uint64_t size = thread->state.arg<1, uint64_t>();
  if (size > FRAME_SIZE) {
    co_return E2BIG;
  }
  if (size < SIZE_VER0) {
    co_return EINVAL;
  }
  // The part Linux knows (missing fields are 0), then the rest must be 0.
  CloneArgs args{};
  const uint64_t known = size < sizeof(args) ? size : sizeof(args);
  if (!co_await VMM::copy_in(thread->process, &args, uargs, known)) {
    co_return EFAULT;
  }
  if (size > known) {
    const std::optional<bool> zero = co_await all_zero(
        thread->process, VA{uargs.va() + known}, size - known);
    if (!zero) {
      co_return EFAULT;
    }
    if (!*zero) {
      co_return E2BIG;
    }
  }
  if (const int64_t error = check_struct(args); error != 0) {
    co_return error;
  }
  // set_tid's pids: Linux reads them all (only the first can be fine here)
  int first_tid = 0;
  if (args.set_tid_size != 0) {
    int tids[MAX_PID_NS_LEVEL];
    if (!co_await VMM::copy_in(thread->process, tids, VA{args.set_tid},
                               args.set_tid_size * sizeof(int))) {
      co_return EFAULT;
    }
    first_tid = tids[0];
  }
  if (const int64_t error = check(args, first_tid); error != 0) {
    co_return error;
  }
  Thread child = make_child(thread, args); // moved into the ready queue last
  if (child == nullptr) {
    co_return EAGAIN;
  }
  const int tid = child->tid;
  // Before the child runs: a waiter mustn't see 0 there too early. Like
  // Linux, a bad address is the parent's problem: the child runs anyway.
  if ((args.flags & CLONE_PARENT_SETTID) != 0) {
    (void)co_await VMM::copy_out(thread->process, VA{args.parent_tid}, &tid,
                                 sizeof(tid));
  }
  std::move(child).schedule();
  co_return tid;
}
