#include "proc/signals.h"
#include "arch/pit.h"
#include "proc/pcb.h"
#include "proc/tcb.h"
namespace {
struct Entry {
  uint64_t pid;
  WeakRef<PCB> pcb;
  bool reaped = false;
  Entry *next = nullptr;
};
SpinLock registry_lock;
Entry *registry = nullptr;
} // namespace
namespace Signals {
void register_process(Process p) {
  // A weak reference avoids keeping zombies' address spaces alive.
  auto e = new Entry{p->pid, {}};
  e->pcb = p.weak();
  LockGuard g{registry_lock};
  e->next = registry;
  registry = e;
}
Process find(int pid) {
  LockGuard g{registry_lock};
  for (auto e = registry; e; e = e->next)
    if (e->pid == uint64_t(pid))
      return Process::promote(e->pcb);
  return {};
}
bool exists(int pid) {
  LockGuard g{registry_lock};
  for (auto e = registry; e; e = e->next)
    if (e->pid == uint64_t(pid))
      return !e->reaped;
  return false;
}
void reap(int pid) {
  LockGuard g{registry_lock};
  for (auto e = registry; e; e = e->next)
    if (e->pid == uint64_t(pid)) {
      e->reaped = true;
      return;
    }
}
bool pending(Thread t) {
  auto p = t->process;
  LockGuard g{p->signal_lock};
  if (p->alarm_at && Pit::jiffies.get() >= p->alarm_at) {
    p->alarm_at = 0;
    p->pending |= 1ULL << 13;
  }
  return t->fault_signal || (p->pending & ~t->blocked);
}
void fault(Thread t, uint64_t address, int code) {
  t->fault_code = code;
  t->fault_signal = 11;
  t->fault_address = address;
  t.schedule();
}
sync<void> deliver(Thread t) {
  auto p = t->process;
  PCB::Action action;
  int sig = 0;
  bool fault = false;
  uint64_t sender = 0;
  {
    LockGuard g{p->signal_lock};
    fault = t->fault_signal != 0;
    if (fault) {
      sig = t->fault_signal;
      t->fault_signal = 0;
    } else
      for (int i = 1; i < 16; ++i)
        if ((p->pending & ~t->blocked) & (1ULL << (i - 1))) {
          sig = i;
          p->pending &= ~(1ULL << (i - 1));
          break;
        }
    if (!sig)
      co_return;
    action = p->actions[sig];
    sender = p->senders[sig];
  }
  if (action.handler == 1 && !fault)
    co_return;
  if (action.handler <= 1 || (fault && (t->blocked & (1ULL << (sig - 1)))) ||
      action.handler >= (1ULL << 47) || !action.restorer ||
      !(action.flags & 0x4000000)) {
    p.exit_group(action.handler <= 1 && !fault ? sig : 11);
    t.exit(0);
    co_return;
  }
  auto frame = new TCB::SignalFrame;
  frame->saved.copy_from(t->state);
  frame->blocked = t->blocked;
  frame->next = t->signal_frames;
  // Red zone plus Linux-compatible ucontext and siginfo, on the user stack.
  uint64_t base = ((t->state.rsp - 128 - 1024) & ~uint64_t(15)) - 8;
  char data[1024]{};
  auto words = reinterpret_cast<uint64_t *>(data);
  words[0] = action.restorer;
  uint64_t *r = reinterpret_cast<uint64_t *>(data + 8 + 40);
  auto &s = t->state;
  r[0] = s.r8;
  r[1] = s.r9;
  r[2] = s.r10;
  r[3] = s.r11;
  r[4] = s.r12;
  r[5] = s.r13;
  r[6] = s.r14;
  r[7] = s.r15;
  r[8] = s.rdi;
  r[9] = s.rsi;
  r[10] = s.rbp;
  r[11] = s.rbx;
  r[12] = s.rdx;
  r[13] = s.rax;
  r[14] = s.rcx;
  r[15] = s.rsp;
  r[16] = s.rip;
  r[17] = s.rflags;
  r[18] = 0x002b000000000033ULL;
  r[19] = fault ? 4 : 0;
  r[20] = fault ? 14 : 0;
  r[21] = t->blocked;
  r[22] = fault ? t->fault_address : 0;
  *reinterpret_cast<uint64_t *>(data + 8 + 296) = t->blocked;
  int *info = reinterpret_cast<int *>(data + 512);
  info[0] = sig;
  info[1] = 0;
  info[2] = fault ? t->fault_code : 0;
  *reinterpret_cast<uint64_t *>(data + 528) = fault ? t->fault_address : sender;
  frame->user_context = base + 8;
  if (!co_await VMM::copy_out(p, VA{base}, data, sizeof(data))) {
    delete frame;
    p.exit_group(11);
    t.exit(0);
    co_return;
  }
  t->signal_frames = frame;
  t->blocked |= action.mask | (1ULL << (sig - 1));
  s.rsp = base;
  s.rip = action.handler;
  s.rdi = sig;
  s.rsi = base + 512;
  s.rdx = base + 8;
  co_return;
}
} // namespace Signals

extern "C" [[noreturn]] void userGeneralProtection() {
  sti();
  Signals::fault(Thread::current(), 0);
  CCB::restart_event_loop();
}
