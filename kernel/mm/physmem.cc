/* Copyright (C) 2025 Ahmed Gheith and contributors.
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

#include "mm/physmem.h"

#include "boot/limine++.h"
#include "boot/limine.h"
#include "lib/print.h"
#include "mm/vmm.h"
#include <cstdint>

using namespace limine;

namespace impl {

struct Range {
  Range *next;
  uint64_t base;
  uint64_t length;
};

[[gnu::section(
    ".limine_requests")]] static constexpr volatile MemMap limine_memmap{};

#if 0
static char const *memmap_type_names[] = {"Usable",
                                          "Reserved",
                                          "AcpiReclaimable",
                                          "AcpiNvs",
                                          "BadMemory",
                                          "BootloaderReclaimable",
                                          "ExecutableAndModules",
                                          "Framebuffer",
                                          "AcpiTables"};
#endif

} // namespace impl

void do_print(const PPN &ppn) {
  do_print("PPN(");
  do_print(ppn.ppn());
  do_print(")");
}

void do_print(const PA &pa) {
  do_print("PA(");
  do_print(pa.pa());
  do_print(")");
}

void do_print(const Offset &offset) {
  do_print("Offset(");
  do_print(offset.offset());
  do_print(")");
}

void PhysMem::init() {
  using namespace impl;

  free_ranges = nullptr;
  ASSERT(limine_memmap.response != nullptr);
  const uint64_t n_entries = limine_memmap.response->entry_count;
  auto const *const *entries = limine_memmap.response->entries;

  // The Ranges live in the first usable frame: there's no heap yet.
  uint64_t stolen = UINT64_MAX;
  for (uint64_t i = 0; i < n_entries; i++) {
    if (entries[i]->type == LIMINE_MEMMAP_USABLE) {
      stolen = entries[i]->base;
      break;
    }
  }
  ASSERT(stolen != UINT64_MAX);
  Range *const ranges = VA{PA{stolen}};

  uint64_t n_ranges = 0;
  for (uint64_t i = 0; i < n_entries; i++) {
    auto const *entry = entries[i];
    if (entry->type != LIMINE_MEMMAP_USABLE) {
      continue;
    }
    uint64_t base = entry->base;
    uint64_t length = entry->length;
    if (base == stolen) {
      base += FRAME_SIZE;
      length -= FRAME_SIZE;
    }
    if (length == 0) {
      continue;
    }
    if (n_ranges == FRAME_SIZE / sizeof(Range)) {
      KPANIC("physmem: more than ? usable ranges\n", Dec(n_ranges));
    }
    Range *range = &ranges[n_ranges++];
    *range = Range{free_ranges, base, length};
    free_ranges = range;
    total_memory += length;
  }

  // Reserve reference counts from boot memory, before the heap exists.
  uint64_t highest = 0;
  for (auto r = free_ranges; r; r = r->next)
    if (r->base + r->length > highest) highest = r->base + r->length;
  reference_count = (highest + FRAME_SIZE - 1) / FRAME_SIZE;
  uint64_t metadata = (reference_count * sizeof(uint32_t) + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);
  Range **candidate = &free_ranges;
  while (*candidate && (*candidate)->length < metadata) candidate = &(*candidate)->next;
  ASSERT(*candidate != nullptr);
  references = (uint32_t *)VA{PA{(*candidate)->base}}.va();
  for (uint64_t i = 0; i < reference_count; ++i) references[i] = 0;
  (*candidate)->base += metadata;
  (*candidate)->length -= metadata;
  if (!(*candidate)->length) *candidate = (*candidate)->next;
  total_memory -= metadata;

  KPRINT("avaialble memory: ?\n", Dec(total_memory));
  auto p = free_ranges;
  while (p) {
    KPRINT("    0x? ?\n", p->base, Dec(p->length / FRAME_SIZE));
    p = p->next;
  }
}

PPN PhysMem::alloc() {
  lock.lock();

  auto frame = avail;
  PA pa{0};

  if (frame != nullptr) {
    // recycled frames hold old data, zeroed below
    avail = frame->next;
    pa = PA{frame->ppn()};
  } else {
    if (free_ranges == nullptr) {
      lock.unlock();
      KPANIC("?\n", "out of frames");
    }

    pa = PA{free_ranges->base};

    free_ranges->base += FRAME_SIZE;
    free_ranges->length -= FRAME_SIZE;
    if (free_ranges->length == 0) {
      free_ranges = free_ranges->next;
    }
  }
  n_allocated += 1;
  ASSERT(pa.ppn().ppn() < reference_count);
  __atomic_store_n(&references[pa.ppn().ppn()], 1, __ATOMIC_RELEASE);

  lock.unlock();

  uint64_t *const ptr = VA(pa);

  for (uint64_t i = 0; i < FRAME_SIZE / sizeof(uint64_t); i++) {
    ptr[i] = 0;
  }
  return PPN{pa};
}

void PhysMem::free(PPN ppn) {
  impl::AvailFrame *frame = VA(PA(ppn));
  ASSERT(ppn.ppn() < reference_count);
  auto previous = __atomic_fetch_sub(&references[ppn.ppn()], 1, __ATOMIC_ACQ_REL);
  ASSERT(previous > 0);
  if (previous != 1) return;
  lock.lock();
  frame->next = avail;
  avail = frame;
  n_allocated -= 1;
  lock.unlock();
}

uint64_t PhysMem::allocated() {
  LockGuard g{lock};
  return n_allocated;
}

constinit PhysMem physMem{};

void PhysMem::retain(PPN ppn) {
  ASSERT(ppn.ppn() < reference_count);
  auto previous = __atomic_fetch_add(&references[ppn.ppn()], 1, __ATOMIC_RELAXED);
  ASSERT(previous > 0);
}

uint32_t PhysMem::owners(PPN ppn) {
  ASSERT(ppn.ppn() < reference_count);
  return __atomic_load_n(&references[ppn.ppn()], __ATOMIC_ACQUIRE);
}
