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

#pragma once

#include "boot/system_main.h"
#include "sched/spin_lock.h"
#include <cstdint>

constexpr static uint64_t LOG_FRAME_SIZE = 12;
constexpr static uint64_t FRAME_SIZE = 1 << LOG_FRAME_SIZE;

class PA;

class Offset {
private:
  static constexpr uint16_t MASK = 0xFFF;
  uint16_t _val;

public:
  inline explicit Offset(const PA &pa);
  inline explicit Offset(const Offset &rhs) : _val(rhs._val) {}
  inline uint16_t offset() const { return _val; }
};

class PPN {
private:
  static constexpr uint64_t MASK = 0xFFFFFFFFFFFF;
  uint64_t _val;

public:
  inline explicit PPN(uint64_t val) : _val(val & MASK) {}
  inline PPN(const PA &pa);
  inline PPN(const PPN &rhs) : _val(rhs._val) {}
  inline PPN &operator=(const PPN &rhs) {
    _val = rhs._val;
    return *this;
  }
  inline uint64_t ppn() const { return _val; }
};

class PA {
private:
  uint64_t _val;

public:
  // constructors
  inline explicit PA(uint64_t val) : _val(val) {}
  inline PA(const PPN &ppn) : _val(ppn.ppn() << LOG_FRAME_SIZE) {}
  inline PA(const PPN &ppn, const Offset &offset)
      : _val(ppn.ppn() << LOG_FRAME_SIZE | offset.offset()) {}
  inline PA(const PA &rhs) : _val(rhs._val) {}

  // getters
  inline uint64_t pa() const { return _val; }
  inline Offset offset() const { return Offset(*this); }
  inline PPN ppn() const { return PPN(*this); }
};

Offset::Offset(const PA &pa) : _val(pa.pa() & MASK) {}
PPN::PPN(const PA &pa) : _val(pa.pa() >> LOG_FRAME_SIZE) {}

extern void do_print(const Offset &offset);
extern void do_print(const PPN &ppn);
extern void do_print(const PA &pa);

namespace impl {
struct Range;

struct AvailFrame {
  AvailFrame *next;

  PPN ppn() {
    return PPN{(uintptr_t(this) - Sys::hhdm_offset) >> LOG_FRAME_SIZE};
  }
};

} // namespace impl
// namespace impl

class PhysMem {
  impl::AvailFrame *avail = nullptr;
  impl::Range *free_ranges = nullptr;
  uint64_t total_memory = 0;
  // Frames handed out and not freed yet.
  uint64_t n_allocated = 0;
  uint32_t *references = nullptr;
  uint64_t reference_count = 0;
  SpinLock lock{};

public:
  // Constructs nothing at run time: physMem is constinit.
  //   Q: Why must physMem work before the global initializers run?
  constexpr PhysMem() {}

  // Takes the usable memory from Limine's memory map.
  //   Q: Why must it run before anything allocates?
  //   Q: Which memory map entries does it use?
  void init();

  PPN alloc();
  void free(PPN ppn);
  void retain(PPN ppn);
  uint32_t owners(PPN ppn);

  // How many frames are allocated right now.
  //   Q: How could you use it to find a leak?
  uint64_t allocated();
};

extern PhysMem physMem;
