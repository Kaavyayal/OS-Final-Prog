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

#include "io/console.h"
#include "mm/vmm.h"
#include "proc/pcb.h"
#include "proc/process.h"
#include "proc/tcb.h"
namespace FileOps {
sync<StrongRef<OpenFile>> get(Process, int);
}

// read(fd, buf, len): stdin only, one character from the console (COM2),
// raw: no echo, no line editing.
//   Q: Why does it return after one character instead of filling buf?
//   Q: What does the thread do while it waits for a key, and its core?
//   Q: Why read the key before touching buf?
//
// BOGUS: the file descriptors are hard-wired (fd 0 is always the console).
// There's no per-process table of open files, so nothing can be opened,
// closed, duplicated or inherited. Replace this with the real thing: look
// fd up in the process's file table and read from whatever file it names.
sync<int64_t> Syscall::read(Thread thread) {
  const unsigned fd = thread->state.arg<0, unsigned>();
  const VA buf = thread->state.arg<1, VA>();
  const uint64_t len = thread->state.arg<2, uint64_t>();
  auto file = co_await FileOps::get(thread->process, int(fd));
  if (file == nullptr || (file->flags & 3) == 1) {
    co_return EBADF;
  }
  if (!user_range(buf.va(), len)) {
    co_return EFAULT;
  }
  // Nothing to read: Linux returns 0 without waiting.
  if (len == 0) {
    co_return 0;
  }
  if (file->kind == 4) {
    auto reader = co_await file->offset;
    char bytes[4096];
    uint64_t n = 0;
    for (;;) {
      if (thread->process->exiting())
        co_return 0;
      {
        LockGuard g{file->pipe->lock};
        auto &p = *file->pipe.operator->();
        n = len < p.size ? len : p.size;
        if (n)
          for (uint64_t i = 0; i < n; ++i)
            bytes[i] = p.bytes[(p.head + i) % 4096];
        else if (!p.writers.get())
          co_return 0;
      }
      if (n)
        break;
      co_await yield();
    }
    if (!co_await VMM::copy_out(thread->process, buf, bytes, n))
      co_return EFAULT;
    {
      LockGuard g{file->pipe->lock};
      auto &p = *file->pipe.operator->();
      p.head = (p.head + n) % 4096;
      p.size -= n;
    }
    co_return n;
  }
  if (file->node != nullptr) {
    if (co_await file->node->is_dir())
      co_return -21;
    auto offset = co_await file->offset;
    uint64_t size = co_await file->node->size_in_bytes();
    if (offset.data() >= size)
      co_return 0;
    uint64_t length = len < size - offset.data() ? len : size - offset.data();
    char bytes[4096];
    uint64_t done = 0;
    while (done < length) {
      uint64_t n = length - done;
      if (n > sizeof(bytes))
        n = sizeof(bytes);
      uint64_t room = 4096 - ((buf.va() + done) & 4095);
      if (n > room)
        n = room;
      auto got = co_await file->node->read_all(offset.data(), n, bytes);
      if (got <= 0)
        break;
      if (!co_await VMM::copy_out(thread->process, VA{buf.va() + done}, bytes,
                                  got))
        co_return done ? int64_t(done) : EFAULT;
      done += got;
      offset.data() += got;
    }
    co_return done;
  }
  const char ch = co_await Console::getch(thread->process);
  if (ch == 4)
    co_return 0;
  const bool copied = co_await VMM::copy_out(thread->process, buf, &ch, 1);
  co_return copied ? 1 : EFAULT;
}
