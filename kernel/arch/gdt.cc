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

#include "arch/gdt.h"

#include "arch/machine.h"
#include "lib/debug.h"

GDT::GDT() {
  const GDTR limine = sgdt();
  ASSERT(uint64_t(limine.limit) + 1 >= N_LIMINE * sizeof(uint64_t));
  const uint64_t *from = (const uint64_t *)limine.base;
  for (uint64_t i = 0; i < N_LIMINE; i++) {
    entries[i] = from[i];
  }
  // sanity check: 64-bit code (L bit) and a present data segment
  ASSERT(((entries[KERNEL_CS / 8] >> 53) & 1) == 1);
  ASSERT(((entries[KERNEL_DS / 8] >> 47) & 1) == 1);

  entries[USER_DS / 8] = 0x00CFF2000000FFFF; // present, DPL 3, writable
  entries[USER_CS / 8] = 0x00AFFA000000FFFF; // present, DPL 3, 64-bit

  // The TSS descriptor takes two entries: present, type 9 (64-bit TSS).
  const uint64_t base = uint64_t(&tss);
  const uint64_t limit = sizeof(TSS) - 1;
  entries[TSS_SEL / 8] =
      (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (uint64_t(0x89) << 40) |
      (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
  entries[TSS_SEL / 8 + 1] = base >> 32;
}

void GDT::load() {
  lgdt(GDTR{sizeof(entries) - 1, uint64_t(entries)});

  // No segment reload: the kernel entries match Limine's.

  ltr(TSS_SEL);
}
