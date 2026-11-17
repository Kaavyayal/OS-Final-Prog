#include "io/console.h"
#include "lib/print.h"
#include "proc/pcb.h"
#include "proc/tcb.h"
#include "syscall/syscall.h"

namespace FileOps {
sync<StrongRef<OpenFile>> get(Process p, int fd) {
  auto t = co_await p->files;
  co_return fd >= 0 && fd < FileTable::LIMIT ? t.data().files[fd]
                                             : StrongRef<OpenFile>{};
}
sync<int64_t> string(Process p, uint64_t address, char *out, uint64_t limit) {
  uint64_t copied = 0;
  while (copied < limit) {
    if (!Syscall::user_range(address, copied + 1))
      co_return -14;
    bool terminated = false;
    uint64_t chunk = 4096 - ((address + copied) & 4095);
    if (chunk > limit - copied)
      chunk = limit - copied;
    auto used =
        co_await VMM::with_user_page(p, VA{address + copied}, chunk, false,
                                     [&](char *bytes, uint64_t, uint64_t n) {
                                       uint64_t i = 0;
                                       do {
                                         out[copied + i] = bytes[i];
                                         if (!bytes[i++]) {
                                           terminated = true;
                                           break;
                                         }
                                       } while (i < n);
                                       return i;
                                     });
    if (!used)
      co_return -14;
    copied += *used;
    if (terminated)
      co_return copied - 1;
  }
  co_return -36;
}
sync<int64_t> lookup(Process p, int fd, const char *path, StrongRef<Node> &out,
                     bool nofollow) {
  if (!*path)
    co_return -2;
  StrongRef<Node> dir;
  {
    auto cwd = co_await p->cwd;
    dir = *path == '/' || cwd.data() == nullptr ? p->fs->root : cwd.data();
  }
  if (*path != '/' && fd != -100) {
    auto file = co_await get(p, fd);
    if (file == nullptr)
      co_return -9;
    if (file->node == nullptr || !co_await file->node->is_dir())
      co_return -20;
    dir = file->node;
  }
  char todo[4096];
  uint64_t length = 0;
  while (path[length])
    ++length;
  if (length >= sizeof(todo))
    co_return -36;
  memcpy(todo, path, length + 1);
  unsigned links = 0;
  uint64_t pos = 0;
  while (true) {
    while (todo[pos] == '/')
      ++pos;
    if (!todo[pos]) {
      out = dir;
      co_return 0;
    }
    if (!co_await dir->is_dir())
      co_return -20;
    char name[256];
    unsigned n = 0;
    while (todo[pos] && todo[pos] != '/') {
      if (n == 255)
        co_return -36;
      name[n++] = todo[pos++];
    }
    name[n] = 0;
    auto ino = co_await dir->find(name);
    if (!ino)
      co_return -2;
    auto node = co_await p->fs->get_node(ino);
    if (co_await node->is_symlink()) {
      if (nofollow && !todo[pos])
        co_return -40;
      if (++links > 40)
        co_return -40;
      char target[4096];
      uint64_t size = co_await node->size_in_bytes();
      if (!size)
        co_return -2;
      if (size >= sizeof(target))
        co_return -36;
      co_await node->get_symbol(target);
      target[size] = 0;
      uint64_t rest = 0;
      while (todo[pos + rest])
        ++rest;
      if (size + rest >= sizeof(todo))
        co_return -36;
      if (size > pos) {
        for (uint64_t i = rest + 1; i > 0; --i)
          todo[size + i - 1] = todo[pos + i - 1];
      } else {
        for (uint64_t i = 0; i <= rest; ++i)
          todo[size + i] = todo[pos + i];
      }
      memcpy(todo, target, size);
      if (todo[0] == '/')
        dir = p->fs->root;
      pos = 0;
    } else {
      dir = node;
      if (todo[pos] == '/' && !co_await dir->is_dir())
        co_return -20;
    }
  }
}
} // namespace FileOps

sync<int64_t> Syscall::filesystem(Thread thread, uint32_t number) {
  auto p = thread->process;
  auto &s = thread->state;
  if (number == 3) {
    int fd = int(s.rdi);
    auto t = co_await p->files;
    if (fd < 0 || fd >= FileTable::LIMIT || t.data().files[fd] == nullptr)
      co_return -9;
    t.data().files[fd] = nullptr;
    t.data().cloexec[fd] = false;
    co_return 0;
  }
  if (number == 8) {
    auto f = co_await FileOps::get(p, int(s.rdi));
    if (f == nullptr)
      co_return -9;
    if (f->kind)
      co_return -29;
    int whence = int(s.rdx);
    if (whence < 0 || whence > 2)
      co_return -22;
    auto offset = co_await f->offset;
    int64_t base = whence == 0   ? 0
                   : whence == 1 ? offset.data()
                                 : co_await f->node->size_in_bytes();
    int64_t delta = int64_t(s.rsi);
    if ((delta > 0 && base > INT64_MAX - delta) || (delta < 0 && delta < -base))
      co_return -22;
    offset.data() = base + delta;
    co_return offset.data();
  }
  int fd = number == 257 ? int(s.rdi) : -100;
  int flags = int(number == 257 ? s.rdx : s.rsi);
  char name[4096];
  int64_t error = co_await FileOps::string(p, number == 257 ? s.rsi : s.rdi,
                                           name, sizeof(name));
  if (error < 0)
    co_return error;
  if ((flags & 3) == 3 || (flags & (64 | 128 | 512 | 1024 | 0x400000)))
    co_return -22;
  StrongRef<Node> node;
  error = co_await FileOps::lookup(p, fd, name, node, flags & 0x20000);
  if (error)
    co_return error;
  if ((flags & 0x10000) && !co_await node->is_dir())
    co_return -20;
  auto file = StrongRef<OpenFile>::make(node, 0, flags);
  auto t = co_await p->files;
  for (int i = 0; i < FileTable::LIMIT; ++i)
    if (t.data().files[i] == nullptr) {
      t.data().files[i] = file;
      t.data().cloexec[i] = flags & 0x80000;
      co_return i;
    }
  co_return -24;
}
