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

#include "io/block_io.h"
#include "io/pci.h"
#include "sched/sync.h"

// A BlockIO over the disk image in QEMU's ivshmem device memory.
//   Q: Where does the image come from? (look for ivshmem in the Makefile)
//   Q: Why is the constructor that takes a PCI::Bar private?
//   Q: What happens to the kernel's writes when QEMU exits?
class RamDisk : public BlockIO {
private:
  char *const data;
  const uint32_t bytes;
  const uint64_t delay;

  RamDisk(PCI::Bar bar, uint64_t delay);

public:
  explicit RamDisk(uint64_t delay);
  ~RamDisk() = default;

  sync<uint32_t> size_in_bytes() override;
  sync<void> read_block(uint32_t block, char *buffer) override;
  sync<void> write_block(uint32_t block, const char *buffer) override;
};