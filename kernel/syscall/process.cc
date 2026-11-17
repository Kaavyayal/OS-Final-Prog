#include "proc/elf.h"
#include "proc/pcb.h"
#include "proc/signals.h"
#include "proc/tcb.h"
#include "syscall/syscall.h"
#include <utility>

// Small owning buffers for the freestanding kernel (no hosted C++ library).
template <class T> class Buffer {
  T *items = nullptr;
  uint64_t count = 0, capacity = 0;

public:
  Buffer() = default;
  Buffer(const Buffer &) = delete;
  Buffer &operator=(const Buffer &) = delete;
  Buffer(Buffer &&b)
      : items(std::exchange(b.items, nullptr)),
        count(std::exchange(b.count, 0)),
        capacity(std::exchange(b.capacity, 0)) {}
  Buffer &operator=(Buffer &&b) {
    if (this != &b) {
      delete[] items;
      items = std::exchange(b.items, nullptr);
      count = std::exchange(b.count, 0);
      capacity = std::exchange(b.capacity, 0);
    }
    return *this;
  }
  ~Buffer() { delete[] items; }
  void push_back(T value) {
    if (count == capacity) {
      uint64_t cap = capacity ? capacity * 2 : 16;
      T *next = new T[cap];
      for (uint64_t i = 0; i < count; ++i)
        next[i] = std::move(items[i]);
      delete[] items;
      items = next;
      capacity = cap;
    }
    items[count++] = std::move(value);
  }
  uint64_t size() const { return count; }
  bool empty() const { return count == 0; }
  T *data() { return items; }
  T *begin() { return items; }
  T *end() { return items + count; }
};
struct Text {
  Buffer<char> bytes;
  void push_back(char c) { bytes.push_back(c); }
  uint64_t size() const { return bytes.size() - 1; }
  const char *c_str() { return bytes.data(); }
};

namespace FileOps {
sync<StrongRef<OpenFile>> get(Process, int);
sync<int64_t> string(Process, uint64_t, char *, uint64_t);
sync<int64_t> lookup(Process, int, const char *, StrongRef<Node> &, bool);
} // namespace FileOps

sync<int64_t> Syscall::fork_process(Thread parent) {
  Thread child = Process::create();
  if (child == nullptr)
    co_return EAGAIN;
  auto p = parent->process, c = child->process;
  c->ppid = p->pid;
  c->parent_exited = p->exited;
  c->fs = p->fs;
  {
    auto cwd = co_await p->cwd;
    c->cwd.unshared() = cwd.data();
  }
  {
    LockGuard g{p->signal_lock};
    for (int i = 0; i < 16; ++i)
      c->actions[i] = p->actions[i];
  }
  child->blocked = parent->blocked;
  auto dest = &child->signal_frames;
  for (auto f = parent->signal_frames; f; f = f->next) {
    auto copy = new TCB::SignalFrame;
    copy->saved.copy_from(f->saved);
    copy->blocked = f->blocked;
    copy->user_context = f->user_context;
    copy->next = nullptr;
    *dest = copy;
    dest = &copy->next;
  }
  co_await VMM::fork_memory(p, c);
  {
    auto src = co_await p->files;
    auto dst = co_await c->files;
    dst.data() = src.data();
  }
  child->state.copy_from(parent->state);
  child->state.rax = 0;
  int pid = c->pid;
  {
    auto children = co_await p->children;
    children.data() = new ChildProcess{pid, c->exited, children.data()};
  }
  child.schedule();
  co_return pid;
}

sync<int64_t> Syscall::wait_process(Thread thread) {
  auto p = thread->process;
  auto &s = thread->state;
  int pid = int(s.rdi), options = int(s.rdx);
  if (pid <= 0 || (options & ~1))
    co_return EINVAL;
  async<int> exited;
  {
    auto children = co_await p->children;
    ChildProcess **link = &children.data();
    while (*link && (*link)->pid != pid)
      link = &(*link)->next;
    if (!*link)
      co_return -10;
    // A completed async can be queried without consuming its result.
    if ((options & 1) && !(*link)->exited.peek())
      co_return 0;
    auto child = *link;
    *link = child->next;
    exited = child->exited;
    delete child;
  }
  while (!exited.peek()) {
    if (p->exiting())
      co_return 0;
    co_await yield();
  }
  int status = co_await exited;
  Signals::reap(pid);
  if (s.rsi && !co_await VMM::copy_out(p, VA{s.rsi}, &status, sizeof(status)))
    co_return EFAULT;
  if (s.r10) {
    char usage[144]{};
    if (!co_await VMM::copy_out(p, VA{s.r10}, usage, sizeof(usage)))
      co_return EFAULT;
  }
  co_return pid;
}

namespace {
sync<int64_t> strings(Process p, uint64_t address, Buffer<Text> &out,
                      uint64_t &total) {
  if (!address)
    co_return 0;
  for (uint64_t i = 0; i < 16384; ++i) {
    uint64_t at;
    if (!Syscall::user_range(address, i * 8 + 8) ||
        !co_await VMM::copy_in(p, &at, VA{address + i * 8}, 8))
      co_return -14;
    if (!at)
      co_return 0;
    Text value;
    for (uint64_t j = 0;; ++j) {
      char ch;
      if (++total > 128 * 1024)
        co_return -7;
      if (!Syscall::user_range(at, j + 1) ||
          !co_await VMM::copy_in(p, &ch, VA{at + j}, 1))
        co_return -14;
      value.push_back(ch);
      if (!ch)
        break;
    }
    out.push_back(std::move(value));
  }
  co_return -7;
}
} // namespace

sync<int64_t> Syscall::exec_process(Thread thread, bool at) {
  auto p = thread->process;
  auto &s = thread->state;
  uint64_t path = at ? s.rsi : s.rdi, argv = at ? s.rdx : s.rsi,
           envp = at ? s.r10 : s.rdx;
  int dirfd = at ? int(s.rdi) : -100;
  int flags = at ? int(s.r8) : 0;
  if (flags & ~(0x1000 | 0x100))
    co_return EINVAL;
  char name[4096];
  int64_t error = co_await FileOps::string(p, path, name, sizeof(name));
  if (error < 0)
    co_return error;
  StrongRef<Node> program;
  if (!name[0] && (flags & 0x1000)) {
    auto f = co_await FileOps::get(p, dirfd);
    if (f == nullptr)
      co_return EBADF;
    program = f->node;
  } else {
    error = co_await FileOps::lookup(p, dirfd, name, program, flags & 0x100);
    if (error)
      co_return error;
  }
  if (program == nullptr || !co_await program->is_file())
    co_return -13;
  Buffer<Text> args, env;
  uint64_t total = 0;
  error = co_await strings(p, argv, args, total);
  if (error)
    co_return error;
  error = co_await strings(p, envp, env, total);
  if (error)
    co_return error;
  if (args.empty()) {
    Text value;
    value.push_back(0);
    args.push_back(std::move(value));
  }
  Thread prepared = Process::create();
  if (prepared == nullptr)
    co_return EAGAIN;
  auto fresh = prepared->process;
  uint64_t entry = co_await ELF::load(fresh, program);
  if (!entry) {
    prepared.exit(0);
    co_return -8;
  }
  constexpr uint64_t stack_size = 1024 * 1024;
  constexpr uint64_t stack_top = 0x7ffffff00000;
  auto stack = co_await VMM::mmap(
      fresh, (void *)(stack_top - stack_size), stack_size, 3,
      VMM::MAP_FIXED | VMM::MAP_PRIVATE | VMM::MAP_ANONYMOUS, {}, 0);
  if (!stack) {
    prepared.exit(0);
    co_return stack.error();
  }
  uint64_t sp = stack_top;
  Buffer<uint64_t> ap, ep;
  for (auto &value : args) {
    sp -= value.size() + 1;
    if (!co_await VMM::copy_out(fresh, VA{sp}, value.c_str(),
                                value.size() + 1)) {
      prepared.exit(0);
      co_return EFAULT;
    }
    ap.push_back(sp);
  }
  for (auto &value : env) {
    sp -= value.size() + 1;
    if (!co_await VMM::copy_out(fresh, VA{sp}, value.c_str(),
                                value.size() + 1)) {
      prepared.exit(0);
      co_return EFAULT;
    }
    ep.push_back(sp);
  }
  Buffer<uint64_t> words;
  words.push_back(ap.size());
  for (auto v : ap)
    words.push_back(v);
  words.push_back(0);
  for (auto v : ep)
    words.push_back(v);
  words.push_back(0);
  words.push_back(0);
  words.push_back(0);
  sp = (sp - words.size() * 8) & ~uint64_t(15);
  if (!co_await VMM::copy_out(fresh, VA{sp}, words.data(), words.size() * 8)) {
    prepared.exit(0);
    co_return EFAULT;
  }
  co_await VMM::replace_memory(p, fresh);
  {
    auto table = co_await p->files;
    for (int i = 0; i < FileTable::LIMIT; ++i)
      if (table.data().cloexec[i]) {
        table.data().files[i] = nullptr;
        table.data().cloexec[i] = false;
      }
  }
  {
    LockGuard g{p->signal_lock};
    for (auto &a : p->actions)
      if (a.handler != 1)
        a = {};
  }
  while (thread->signal_frames) {
    auto f = thread->signal_frames;
    thread->signal_frames = f->next;
    delete f;
  }
  SavedState clean;
  s.copy_from(clean);
  s.rip = entry;
  s.rsp = sp;
  prepared.exit(0);
  co_return 0;
}
