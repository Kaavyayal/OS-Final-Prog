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

#include "io/ramdisk.h"
#include "lib/print.h"
#include "mm/vmm.h"
#include "sched/async.h"
#include "sched/sync.h"

// QEMU's ivshmem device: its PCI ids and the BAR holding the shared memory.
//   Q: Where else can you see these ids? (try info pci in the QEMU monitor)
//   Q: What would happen if two ivshmem devices were attached?
constexpr uint16_t IVSHMEM_VENDOR_ID = 0x1AF4;
constexpr uint16_t IVSHMEM_DEVICE_ID = 0x1110;
constexpr uint32_t IVSHMEM_MEMORY_BAR = 2;

static PCI::Bar find_ivshmem() {
  auto ivshmem = PCI::find(IVSHMEM_VENDOR_ID, IVSHMEM_DEVICE_ID);
  if (!ivshmem) {
    KPANIC("? not found\n", "the ramdisk (ivshmem device)");
  }
  return ivshmem->bar(IVSHMEM_MEMORY_BAR);
}

RamDisk::RamDisk(PCI::Bar bar, uint64_t delay)
    : BlockIO(512), data((char *)VMM::map_device(bar.pa, bar.bytes).va()),
      bytes(uint32_t(bar.bytes)), delay(delay) {
  ASSERT(bar.bytes <= UINT32_MAX);
}

RamDisk::RamDisk(uint64_t delay) : RamDisk(find_ivshmem(), delay) {}

sync<void> RamDisk::read_block(uint32_t block, char *buffer) {
  ASSERT(block < co_await size_in_blocks());
  for (uint64_t i = 0; i < delay; i++) {
    // Waste time to simulate a slow disk.
    co_await yield();
  }

  auto offset = block_size * block;
  memcpy(buffer, data + offset, block_size);
}

sync<void> RamDisk::write_block(uint32_t block, const char *buffer) {
  ASSERT(block < co_await size_in_blocks());
  for (uint64_t i = 0; i < delay; i++) {
    // Waste time to simulate a slow disk.
    co_await yield();
  }

  auto offset = block_size * block;
  memcpy(data + offset, buffer, block_size);
}

sync<uint32_t> RamDisk::size_in_bytes() { co_return bytes; }
