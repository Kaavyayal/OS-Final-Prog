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

#include "io/pci.h"
#include "arch/machine.h"
#include "sched/spin_lock.h"

namespace PCI {

constexpr uint16_t CONFIG_ADDRESS = 0xCF8;
constexpr uint16_t CONFIG_DATA = 0xCFC;

constexpr uint32_t COMMAND = 0x04;
constexpr uint32_t COMMAND_MEMORY = 1 << 1; // decodes its memory BARs
constexpr uint32_t HEADER_TYPE = 0x0C;      // byte 2 of the register
constexpr uint32_t MULTI_FUNCTION = 1 << 23;
constexpr uint32_t BAR0 = 0x10;

// Makes the address write and the data access one step.
//   Q: What could another core do between the two port accesses without it?
static SpinLock config_lock{};

Function::Function(uint32_t bus, uint32_t device, uint32_t function)
    : address((UINT32_C(1) << 31) | (bus << 16) | (device << 11) |
              (function << 8)) {
  ASSERT(bus < 256 && device < 32 && function < 8);
}

uint32_t Function::read(uint32_t offset) const {
  ASSERT(offset < 256 && offset % 4 == 0);
  LockGuard g{config_lock};
  outl(CONFIG_ADDRESS, address | offset);
  return inl(CONFIG_DATA);
}

void Function::write(uint32_t offset, uint32_t value) const {
  ASSERT(offset < 256 && offset % 4 == 0);
  LockGuard g{config_lock};
  outl(CONFIG_ADDRESS, address | offset);
  outl(CONFIG_DATA, value);
}

// Reads BAR n's address and measures its size.
//   Q: How does writing all ones reveal the size?
//   Q: Why is memory decoding off while the BAR holds all ones?
//   Q: Why is only the low half of the COMMAND register written?
Bar Function::bar(uint32_t n) const {
  ASSERT(n < 6);
  const uint32_t offset = BAR0 + 4 * n;
  const uint32_t low = read(offset);
  ASSERT((low & 1) == 0); // memory, not I/O ports
  const bool is_64 = ((low >> 1) & 3) == 2;
  ASSERT(!is_64 || n < 5);

  const uint32_t command = read(COMMAND) & 0xFFFF;
  write(COMMAND, command & ~COMMAND_MEMORY);

  uint64_t base = low & ~UINT32_C(0xF);
  write(offset, ~UINT32_C(0));
  uint64_t mask = read(offset) & ~UINT32_C(0xF);
  write(offset, low);
  if (is_64) {
    const uint32_t high = read(offset + 4);
    base |= uint64_t(high) << 32;
    write(offset + 4, ~UINT32_C(0));
    mask |= uint64_t(read(offset + 4)) << 32;
    write(offset + 4, high);
  } else {
    mask |= UINT64_C(0xFFFFFFFF00000000);
  }

  write(COMMAND, command);
  return Bar{PA{base}, ~mask + 1};
}

std::optional<Function> find(uint16_t vendor_id, uint16_t device_id) {
  for (uint32_t bus = 0; bus < 256; bus++) {
    for (uint32_t device = 0; device < 32; device++) {
      // function 0 tells whether the device is there and has others
      const Function first{bus, device, 0};
      if (first.vendor_id() == 0xFFFF) {
        continue;
      }
      const bool multi = (first.read(HEADER_TYPE) & MULTI_FUNCTION) != 0;
      for (uint32_t function = 0; function < (multi ? 8u : 1u); function++) {
        const Function f{bus, device, function};
        if (f.vendor_id() == vendor_id && f.device_id() == device_id) {
          return f;
        }
      }
    }
  }
  return std::nullopt;
}

} // namespace PCI
