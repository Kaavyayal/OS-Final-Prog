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

#pragma once

#include <cstdint>

// Per-core GDT and TSS.
//   Q: Why does each core need its own TSS?
//   Q: Which entries come from Limine, and which does the kernel add?
//   Q: Why must user data come right before user code?
class GDT {
public:
  static constexpr uint16_t KERNEL_CS = 0x28;
  static constexpr uint16_t KERNEL_DS = 0x30;
  static constexpr uint16_t USER_DS = 0x38 | 3;
  static constexpr uint16_t USER_CS = 0x40 | 3;
  static constexpr uint16_t TSS_SEL = 0x48;

  // The 64-bit TSS.
  //   Q: Which of its fields does this kernel use?
  struct [[gnu::packed]] TSS {
    uint32_t reserved0 = 0;
    uint64_t rsp[3] = {}; // stacks for entering rings 0..2
    uint64_t reserved1 = 0;
    uint64_t ist[7] = {}; // interrupt stack table
    uint64_t reserved2 = 0;
    uint16_t reserved3 = 0;
    uint16_t iopb = sizeof(TSS); // no I/O permission bitmap
  };
  static_assert(sizeof(TSS) == 104);

  static constexpr uint64_t N_LIMINE = 7; // entries copied from Limine

  // The descriptors; public so the CCB stays standard-layout.
  uint64_t entries[N_LIMINE + 4];

  // Its rsp[0] is the stack for interrupts and exceptions from user mode.
  //   Q: Why must it be set before entering user mode?
  TSS tss{};

  // Copies the kernel entries from Limine's GDT.
  GDT();
  GDT(const GDT &) = delete;
  GDT &operator=(const GDT &) = delete;

  // Loads this GDT and its TSS on the current core.
  //   Q: Why don't the segment registers need reloading?
  void load();
};
