#include "arch/pit.h"
#include "proc/pcb.h"
#include "proc/signals.h"
#include "proc/tcb.h"
#include "syscall/syscall.h"
namespace FileOps {
sync<StrongRef<OpenFile>> get(Process, int);
sync<int64_t> string(Process, uint64_t, char *, uint64_t);
sync<int64_t> lookup(Process, int, const char *, StrongRef<Node> &, bool);
} // namespace FileOps
namespace {
struct Waiter {
  Process process;
  uint64_t address;
  async_setter<int> setter;
  Waiter *next;
};
Mutex<Waiter *> waiters{nullptr};
sync<int> wake_waiters(Process p, uint64_t address, int count) {
  auto guard = co_await waiters;
  int n = 0;
  auto link = &guard.data();
  while (*link && n < count) {
    auto w = *link;
    if (w->process == p && w->address == address) {
      *link = w->next;
      std::move(w->setter).set(0);
      ++n;
    } else
      link = &w->next;
  }
  co_return n;
}
sync<void> cancel_waiters(Process p) {
  auto guard = co_await waiters;
  auto link = &guard.data();
  while (*link) {
    auto w = *link;
    if (w->process == p) {
      *link = w->next;
      std::move(w->setter).set(0);
    } else
      link = &w->next;
  }
}

} // namespace
namespace Futex {
void cancel(Process p) { (void)cancel_waiters(p); }
void wake(Process p, uint64_t address, int count) {
  (void)wake_waiters(p, address, count);
}
} // namespace Futex
sync<int64_t> Syscall::extended(Thread t, uint32_t number) {
  auto p = t->process;
  auto &s = t->state;
  if (number == 292) {
    int oldfd = int(s.rdi), newfd = int(s.rsi);
    if ((s.rdx & ~uint64_t(0x80000)) || oldfd == newfd)
      co_return EINVAL;
    auto table = co_await p->files;
    if (oldfd < 0 || newfd < 0 || oldfd >= FileTable::LIMIT ||
        newfd >= FileTable::LIMIT || table.data().files[oldfd] == nullptr)
      co_return EBADF;
    table.data().files[newfd] = table.data().files[oldfd];
    table.data().cloexec[newfd] = s.rdx & 0x80000;
    co_return newfd;
  }
  if (number == 293) {
    if (s.rsi & ~uint64_t(0x80000))
      co_return EINVAL;
    auto table = co_await p->files;
    int fd[2] = {-1, -1};
    for (int i = 0; i < FileTable::LIMIT; ++i)
      if (table.data().files[i] == nullptr) {
        if (fd[0] < 0)
          fd[0] = i;
        else {
          fd[1] = i;
          break;
        }
      }
    if (fd[1] < 0)
      co_return -24;
    if (!co_await VMM::copy_out(p, VA{s.rdi}, fd, sizeof(fd)))
      co_return EFAULT;
    auto pipe = StrongRef<Pipe>::make();
    for (int i = 0; i < 2; ++i) {
      auto f = StrongRef<OpenFile>::make(StrongRef<Node>{}, 4 + i, i);
      f->pipe = pipe;
      table.data().files[fd[i]] = f;
      table.data().cloexec[fd[i]] = s.rsi & 0x80000;
    }
    co_return 0;
  }
  if (number == 80) {
    char path[4096];
    auto error = co_await FileOps::string(p, s.rdi, path, sizeof(path));
    if (error < 0)
      co_return error;
    StrongRef<Node> dir;
    error = co_await FileOps::lookup(p, -100, path, dir, false);
    if (error)
      co_return error;
    if (!co_await dir->is_dir())
      co_return -20;
    auto cwd = co_await p->cwd;
    cwd.data() = dir;
    co_return 0;
  }
  if (number == 79) {
    char path[4096];
    unsigned at = 4095;
    path[at] = 0;
    StrongRef<Node> dir;
    {
      auto cwd = co_await p->cwd;
      dir = cwd.data() == nullptr ? p->fs->root : cwd.data();
    }
    while (dir->number != p->fs->root->number) {
      auto parent = co_await p->fs->get_node(co_await dir->find(".."));
      char name[256]{};
      co_await parent->entries([&](uint32_t ino, const char *n) {
        if (ino == dir->number &&
            !(n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])))) {
          unsigned i = 0;
          while (n[i] && i < 255) {
            name[i] = n[i];
            ++i;
          }
          name[i] = 0;
        }
      });
      unsigned n = 0;
      while (name[n])
        ++n;
      if (!n)
        co_return -2;
      if (at < n + 1)
        co_return -36;
      at -= n;
      memcpy(path + at, name, n);
      path[--at] = '/';
      dir = parent;
    }
    if (at == 4095)
      path[--at] = '/';
    uint64_t len = 4096 - at;
    if (s.rsi < len)
      co_return -34;
    if (!co_await VMM::copy_out(p, VA{s.rdi}, path + at, len))
      co_return EFAULT;
    co_return len;
  }
  if (number == 35) {
    struct {
      int64_t sec, nsec;
    } req;
    if (!co_await VMM::copy_in(p, &req, VA{s.rdi}, sizeof(req)))
      co_return EFAULT;
    if (req.sec < 0 || req.nsec < 0 || req.nsec >= 1000000000)
      co_return EINVAL;
    uint64_t hz = Pit::jiffies_per_second;
    uint64_t ticks =
        uint64_t(req.sec) > UINT64_MAX / hz - 2
            ? UINT64_MAX / 2
            : uint64_t(req.sec) * hz + (req.nsec * hz + 999999999) / 1000000000;
    uint64_t start = Pit::jiffies.get();
    while (ticks && Pit::jiffies.get() - start < ticks + 1) {
      if (p->exiting())
        co_return 0;
      co_await yield();
    }
    co_return 0;
  }
  if (number == 202) {
    int op = int(s.rsi);
    if (op != 0 && op != 1 && op != 128 && op != 129)
      co_return EINVAL;
    if ((op & 127) == 0 && s.r10)
      co_return EINVAL;
    if (s.rdi & 3)
      co_return EINVAL;
    if ((op & 127) == 1) {
      if (int(s.rdx) < 0)
        co_return EINVAL;
      // WAKE still validates the aligned, readable user word.
      uint32_t word;
      if (!co_await VMM::copy_in(p, &word, VA{s.rdi}, 4))
        co_return EFAULT;
      co_return co_await wake_waiters(p, s.rdi, int(s.rdx));
    }
    Waiter w{p, s.rdi, {}, nullptr};
    auto result = w.setter.result();
    {
      auto lock = co_await waiters;
      uint32_t word;
      if (p->exiting())
        co_return 0;
      if (!co_await VMM::copy_in(p, &word, VA{s.rdi}, 4))
        co_return EFAULT;
      if (word != uint32_t(s.rdx))
        co_return EAGAIN;
      w.next = lock.data();
      lock.data() = &w;
    }
    co_return co_await result;
  }
  if (number == 37) {
    uint64_t now = Pit::jiffies.get(), hz = Pit::jiffies_per_second;
    LockGuard g{p->signal_lock};
    uint64_t old = p->alarm_at;
    p->alarm_at = uint32_t(s.rdi) ? now + uint64_t(uint32_t(s.rdi)) * hz : 0;
    co_return old > now ? (old - now + hz - 1) / hz : 0;
  }
  if (number == 62) {
    int pid = int(s.rdi), sig = int(s.rsi);
    if (pid <= 0 || (sig != 0 && sig != 2 && sig != 10 && sig != 11 &&
                     sig != 14 && sig != 15))
      co_return EINVAL;
    if (!Signals::exists(pid))
      co_return -3;
    auto target = Signals::find(pid);
    if (target == nullptr || target->exiting())
      co_return 0;
    if (sig) {
      LockGuard g{target->signal_lock};
      target->pending |= 1ULL << (sig - 1);
      target->senders[sig] = p->pid;
    }
    co_return 0;
  }
  if (number == 13) {
    int sig = int(s.rdi);
    if (s.r10 != 8 ||
        (sig != 2 && sig != 10 && sig != 11 && sig != 14 && sig != 15))
      co_return EINVAL;
    if (s.rsi) {
      PCB::Action a;
      if (!co_await VMM::copy_in(p, &a, VA{s.rsi}, sizeof(a)))
        co_return EFAULT;
      if (a.flags & ~uint64_t(0x4 | 0x4000000 | 0x10000000))
        co_return EINVAL;
      LockGuard g{p->signal_lock};
      p->actions[sig] = a;
    }
    co_return 0;
  }
  if (number == 15) {
    auto frame = t->signal_frames;
    if (!frame) {
      p.exit_group(11);
      co_return 0;
    }
    // Linux ucontext starts with flags/link/stack, then sigcontext at +40.
    uint64_t regs[23];
    if (!co_await VMM::copy_in(p, regs, VA{frame->user_context + 40},
                               sizeof(regs))) {
      p.exit_group(11);
      co_return 0;
    }
    s.copy_from(frame->saved);
    s.r8 = regs[0];
    s.r9 = regs[1];
    s.r10 = regs[2];
    s.r11 = regs[3];
    s.r12 = regs[4];
    s.r13 = regs[5];
    s.r14 = regs[6];
    s.r15 = regs[7];
    s.rdi = regs[8];
    s.rsi = regs[9];
    s.rbp = regs[10];
    s.rbx = regs[11];
    s.rdx = regs[12];
    s.rax = regs[13];
    s.rcx = regs[14];
    s.rsp = regs[15];
    s.rip = regs[16];
    s.rflags = regs[17];
    t->blocked = frame->blocked;
    t->signal_frames = frame->next;
    delete frame;
    if (s.rip >= USER_END || s.rsp >= USER_END) {
      p.exit_group(11);
      co_return 0;
    }
    co_return int64_t(s.rax);
  }
  co_return ENOSYS;
}
