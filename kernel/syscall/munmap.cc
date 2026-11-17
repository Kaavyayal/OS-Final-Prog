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

#include "mm/vmm.h"
#include "proc/process.h"
#include "proc/tcb.h"

// munmap in the process's address space.
//   Q: What does unmapping a range with nothing in it return?
//   Q: When does it wait?
sync<int64_t> Syscall::munmap(Thread thread) {
  const uint64_t addr = thread->state.arg<0, uint64_t>();
  const uint64_t length = thread->state.arg<1, uint64_t>();
  const int status =
      co_await VMM::munmap(thread->process, (void *)addr, length);
  co_return status != 0 ? EINVAL : 0;
}
