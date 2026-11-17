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
#include "proc/pcb.h"
namespace FileOps { sync<StrongRef<OpenFile>> get(Process,int); }

// mmap in the process's address space: anonymous or file-backed.
// File-backed shared writable mappings require a read/write descriptor.
//   Q: Why is the offset checked before anything else?
//   Q: When does it wait?
sync<int64_t> Syscall::mmap(Thread thread) {
  const uint64_t addr = thread->state.arg<0, uint64_t>();
  const uint64_t length = thread->state.arg<1, uint64_t>();
  const uint64_t prot = thread->state.arg<2, uint64_t>();
  const uint64_t flags = thread->state.arg<3, uint64_t>();
  // Argument 4 is the file descriptor; anonymous mappings ignore it.
  const uint64_t offset = thread->state.arg<5, uint64_t>();
  // Linux checks these two first (in the system call itself)
  if ((offset & (FRAME_SIZE - 1)) != 0) {
    co_return EINVAL;
  }
  StrongRef<Node> node;
  if ((flags & VMM::MAP_ANONYMOUS) == 0) {
    auto file = co_await FileOps::get(thread->process, int(thread->state.r8));
    if (file == nullptr) co_return EBADF;
    if ((file->flags & 3) == 1) co_return -13;
    if (file->node == nullptr || !co_await file->node->is_file()) co_return -19;
    if ((flags & VMM::MAP_TYPE) == VMM::MAP_SHARED &&
        (prot & VMM::PROT_WRITE) && (file->flags & 3) != 2) co_return -13;
    node = file->node;
  }
  const std::expected<void *, int64_t> p =
      co_await VMM::mmap(thread->process, (void *)addr, length, int(prot),
                         int(flags), node, offset);
  co_return p ? int64_t(*p) : p.error();
}
