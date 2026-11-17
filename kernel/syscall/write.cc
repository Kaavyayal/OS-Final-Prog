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
#include "lib/debug.h"
#include "lib/print.h"
#include "mm/vmm.h"
#include "proc/pcb.h"
#include "proc/process.h"
#include "proc/tcb.h"
namespace FileOps {
sync<StrongRef<OpenFile>> get(Process, int);
}

namespace {

// The most one write sends: the rest is a short write.
//   Q: Why copy the bytes in instead of sending them from the user's page?
//   Q: Why does a bigger MAX_WRITE make the coroutine's frame bigger?
constexpr uint64_t MAX_WRITE = 256;

// stderr: the debug output (COM1), with the kernel's own prints, under
// KPRINT's lock.
//   Q: Why hold the lock for the whole write?
//   Q: Why is busy-waiting (putch) acceptable here but not for stdout?
void debug_write(const char *bytes, uint64_t n) {
  impl::print_lock();
  for (uint64_t i = 0; i < n; i++) {
    putch(bytes[i]);
  }
  impl::print_unlock();
}

} // namespace

// write(fd, buf, len): stdout (1) goes to the console (COM2), stderr (2) to
// the debug output (COM1).
//   Q: How much of buf does it send?
//   Q: What does it return for any other fd?
//
// BOGUS: the file descriptors are hard-wired (fd 1 is always the console,
// fd 2 the debug output). There's no per-process table of open files, so
// nothing can be opened, closed, duplicated or redirected. Replace this with
// the real thing: look fd up in the process's file table and write to
// whatever file it names.
sync<int64_t> Syscall::write(Thread thread) {
  const unsigned fd = thread->state.arg<0, unsigned>();
  const VA buf = thread->state.arg<1, VA>();
  const uint64_t len = thread->state.arg<2, uint64_t>();
  auto file = co_await FileOps::get(thread->process, int(fd));
  if (file == nullptr || (file->flags & 3) == 0) {
    co_return EBADF;
  }
  if (!user_range(buf.va(), len)) {
    co_return EFAULT;
  }

  if (len == 0) {
    co_return 0;
  }
  const uint64_t n = len < MAX_WRITE ? len : MAX_WRITE;
  char bytes[MAX_WRITE];
  if (!co_await VMM::copy_in(thread->process, bytes, buf, n)) {
    co_return EFAULT;
  }
  if (file->kind == 5) {
    uint64_t done = 0;
    while (done < len) {
      uint64_t count = len - done;
      if (count > MAX_WRITE)
        count = MAX_WRITE;
      if (done && !co_await VMM::copy_in(thread->process, bytes,
                                         VA{buf.va() + done}, count))
        co_return done;
      bool written = false;
      while (!written) {
        if (thread->process->exiting())
          co_return done;
        {
          LockGuard g{file->pipe->lock};
          auto &p = *file->pipe.operator->();
          if (!p.readers.get())
            co_return done ? int64_t(done) : -32;
          if (4096 - p.size >= count) {
            for (uint64_t i = 0; i < count; ++i)
              p.bytes[(p.head + p.size + i) % 4096] = bytes[i];
            p.size += count;
            written = true;
          }
        }
        if (!written)
          co_await yield();
      }
      done += count;
    }
    co_return done;
  }
  if (file->kind == 0) {
    auto offset = co_await file->offset;
    auto written = co_await file->node->write_cached(offset.data(), n, bytes);
    if (written > 0) offset.data() += written;
    co_return written;
  }
  if (file->kind == 2) {
    co_await Console::write(bytes, n);
  } else {
    debug_write(bytes, n);
  }
  co_return int64_t(n);
}
