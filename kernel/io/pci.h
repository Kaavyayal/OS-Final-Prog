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

#include "mm/physmem.h"
#include <cstdint>
#include <optional>

// PCI configuration space through the legacy ports 0xCF8 and 0xCFC.
//   Q: How does a Function's address pick its bus, device and function?
//   Q: Who assigned the BAR addresses before the kernel started?
//   Q: How big is each function's configuration space through these ports?
namespace PCI {

// A memory BAR's physical address and size.
//   Q: Why is the size always a power of two?
struct Bar {
  PA pa;
  uint64_t bytes;
};

class Function {
  uint32_t address; // CONFIG_ADDRESS without the register offset

public:
  Function(uint32_t bus, uint32_t device, uint32_t function);

  // Reads or writes the 4-byte register at offset.
  //   Q: Why must offset be a multiple of 4?
  //   Q: What's in the register at offset 0?
  uint32_t read(uint32_t offset) const;
  void write(uint32_t offset, uint32_t value) const;

  uint16_t vendor_id() const { return uint16_t(read(0x00)); }
  uint16_t device_id() const { return uint16_t(read(0x00) >> 16); }

  // Memory BAR n (0..5), 32 or 64 bits.
  //   Q: Why can't a 64-bit BAR be BAR 5?
  Bar bar(uint32_t n) const;
};

// The first function with these ids, on any bus.
//   Q: How many configuration reads can a full scan take?
//   Q: Why does it check function 0 before the others?
std::optional<Function> find(uint16_t vendor_id, uint16_t device_id);

} // namespace PCI
