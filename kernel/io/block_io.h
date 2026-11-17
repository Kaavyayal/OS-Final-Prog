#pragma once

#include "lib/debug.h"
#include "sched/sync.h"
#include <stdint.h>

// Base class for anything read and written in fixed-size blocks.
//   Q: Which classes implement it, and what must each one provide?
//   Q: Who checks that block numbers and offsets are in bounds?
//   Q: What happens if two coroutines use the same object at once?
class BlockIO {
public:
  const uint32_t block_size;
  BlockIO(uint32_t block_size) : block_size(block_size) {}
  virtual ~BlockIO() {}

  // The number of bytes it holds.
  //   Q: Why is this a coroutine and not a plain function?
  virtual sync<uint32_t> size_in_bytes() = 0;

  // The number of blocks, the last one possibly partial.
  //   Q: Why round up instead of down?
  sync<uint32_t> size_in_blocks() {
    co_return ((co_await size_in_bytes() + block_size - 1) / block_size);
  }

  // Reads block block_number into buffer.
  //   Q: How big must buffer be?
  //   Q: Which implementations wait, and for what?
  virtual sync<void> read_block(uint32_t block_number, char *buffer) = 0;

  // Writes buffer to block block_number.
  //   Q: In a BlockCache, when do the bytes reach the device?
  //   Q: Why doesn't ext2's Node support it yet?
  virtual sync<void> write_block(uint32_t block_number, const char *buffer) = 0;

  // Reads up to n bytes at offset: returns the count, 0 at the end, -1 past it.
  //   Q: Why can it return fewer than n bytes before the end?
  //   Q: When does it read straight into buffer, and when through a copy?
  virtual sync<int64_t> read(uint32_t offset, uint32_t n, char *buffer);

  // Reads n bytes at offset (fewer at the end) by calling read until done.
  //   Q: How many read_block calls does a read across three blocks make?
  //   Q: What does it return when offset is past the end?
  virtual sync<int64_t> read_all(uint32_t offset, uint32_t n, char *buffer);

  // Reads a T at offset.
  //   Q: What happens if fewer than sizeof(T) bytes are left?
  //   Q: Where does ext2 use it?
  template <typename T> sync<void> read(uint32_t offset, T &thing) {
    auto cnt = co_await read_all(offset, sizeof(T), (char *)&thing);
    ASSERT(cnt == sizeof(T));
  }
};
