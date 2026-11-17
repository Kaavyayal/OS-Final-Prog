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

// A minimal ELF loader for statically linked x86-64 programs.
//   Q: What kinds of ELF files does it reject?
//   Q: Which sanity checks does it do on each segment?

#include "proc/elf.h"

#include "lib/shared.h"
#include "mm/physmem.h"
#include "mm/vmm.h"
#include "proc/process.h"

namespace {

uint64_t round_down(uint64_t x) { return x & ~(FRAME_SIZE - 1); }

// x <= USER_END, so this can't overflow.
uint64_t round_up(uint64_t x) { return round_down(x + FRAME_SIZE - 1); }

struct ProgramHeader {

  static constexpr int RWX = VMM::PROT_READ | VMM::PROT_WRITE | VMM::PROT_EXEC;

  static constexpr uint32_t PT_LOAD = 1;
  struct [[gnu::packed]] Data {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
  } data;

  static_assert(sizeof(Data) == 56);

  bool good_segment(const uint64_t file_size) const {
    return data.filesz <= data.memsz && data.vaddr >= VA_START.va() &&
           data.vaddr < VA_END.va() && data.memsz <= VA_END.va() - data.vaddr &&
           // the file bytes it takes are in the file (none: like Linux, the
           // offset doesn't matter, the linker can put it past the end)
           (data.filesz == 0 || (data.offset <= file_size &&
                                 data.filesz <= file_size - data.offset)) &&
           // A file page lands on a virtual page, as mmap needs.
           (data.vaddr % FRAME_SIZE) == (data.offset % FRAME_SIZE);
  }

  uint64_t first_page() const { return round_down(data.vaddr); }

  uint64_t end_page() const { return round_up(data.vaddr + data.memsz); }

  sync<bool> load(Process process, StrongRef<Node> file,
                  uint64_t file_size) const {

    if (data.type != PT_LOAD)
      co_return true;
    if (!good_segment(file_size))
      co_return false;
    if (data.memsz == 0)
      co_return true;

    const uint64_t start = first_page();
    const uint64_t data_end = data.vaddr + data.filesz;
    const uint64_t file_pages_end = round_up(data_end);
    const uint64_t end = end_page();

    if (data.filesz != 0) {
      const uint64_t length = file_pages_end - start;
      const auto actual = co_await VMM::mmap(
          process, (void *)start, length, RWX,
          VMM::MAP_PRIVATE | VMM::MAP_FIXED, file, round_down(data.offset));
      if (!actual)
        co_return false;

      // Zero the rest of the last file page: .bss may start there.
      static const char zeros[FRAME_SIZE] = {};
      const uint64_t tail = file_pages_end - data_end;
      if (!co_await VMM::copy_out(process, VA{data_end}, zeros, tail)) {
        co_return false;
      }
    }

    const uint64_t anon_start = data.filesz != 0 ? file_pages_end : start;
    if (end > anon_start) {
      const uint64_t length = end - anon_start;
      const auto actual = co_await VMM::mmap(
          process, (void *)anon_start, length, RWX,
          VMM::MAP_PRIVATE | VMM::MAP_ANONYMOUS | VMM::MAP_FIXED, {}, 0);
      if (!actual)
        co_return false;
    }

    int prot = ((data.flags & 4) ? VMM::PROT_READ : 0) |
               ((data.flags & 2) ? VMM::PROT_WRITE : 0) |
               ((data.flags & 1) ? VMM::PROT_EXEC : 0);
    co_await VMM::protect(process, start, end - start, prot);
    co_return true;
  }
};

struct Header {

  static constexpr uint16_t ET_EXEC = 2;
  static constexpr uint16_t EM_X86_64 = 62;

  struct [[gnu::packed]] Data {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
  } data;

  static_assert(sizeof(Data) == 64);

  bool good_header() {
    return data.ident[0] == 0x7F && data.ident[1] == 'E' &&
           data.ident[2] == 'L' && data.ident[3] == 'F' &&
           data.ident[4] == 2 /* 64-bit */ &&
           data.ident[5] == 1 /* little-endian */ && data.type == ET_EXEC &&
           data.machine == EM_X86_64 &&
           data.phentsize == sizeof(ProgramHeader) && data.phnum > 0;
  }
};

} // namespace

sync<uint64_t> ELF::load(Process process, StrongRef<Node> file) {
  const uint64_t file_size = co_await file->size_in_bytes();

  // The header, then the program headers it points to.
  Header h{};
  if (file_size < sizeof(h.data) ||
      co_await file->read_all(0, sizeof(h.data), (char *)&h.data) !=
          int64_t(sizeof(h.data)) ||
      !h.good_header()) {
    co_return 0;
  }
  const uint64_t ph_bytes = uint64_t(h.data.phnum) * h.data.phentsize;
  if (h.data.phoff > file_size || ph_bytes > file_size - h.data.phoff) {
    co_return 0;
  }

  bool executable_entry = false;
  for (uint64_t i = 0; i < h.data.phnum; i++) {
    ProgramHeader ph{};
    const uint64_t offset = h.data.phoff + i * h.data.phentsize;
    if (co_await file->read_all(uint32_t(offset), sizeof(ph.data),
                                (char *)&ph.data) != int64_t(sizeof(ph.data)) ||
        !co_await ph.load(process, file, file_size)) {
      co_return 0;
    }
    if (ph.data.type == ProgramHeader::PT_LOAD && (ph.data.flags & 1) &&
        h.data.entry >= ph.data.vaddr && h.data.entry - ph.data.vaddr < ph.data.memsz)
      executable_entry = true;
  }
  co_return executable_entry ? h.data.entry : 0;
}
