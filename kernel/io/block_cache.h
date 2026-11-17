#pragma once

#include "io/block_io.h"
#include "lib/shared.h"
#include "sched/mutex.h"

// A direct-mapped, write-back cache of another BlockIO's blocks.
//   Q: With 31 slots, which slot holds block 40? Which blocks compete for it?
//   Q: When does a written block reach the device?
//   Q: Why does each slot have a Mutex rather than a SpinLock?
class BlockCache : public BlockIO {

  // One cached block.
  struct Slot {
    char *bytes = nullptr; // nullptr => empty
    uint32_t block_number = 0;
    bool dirty = false; // the device's copy is older
  };

  StrongRef<BlockIO> device;
  const uint32_t n_slots;
  Mutex<Slot> *const slots;

  // Makes the locked slot hold block_number, writing back the block it evicts.
  //   Q: How does the signature make sure the caller holds the slot's lock?
  //   Q: Why does write_block pass read = false?
  //   Q: What would go wrong if dirty were cleared before the write-back?
  sync<void> load(Mutex<Slot>::Proof &proof, uint32_t block_number, bool read) {
    Slot &slot = proof.data();
    if (slot.bytes != nullptr && slot.block_number == block_number) {
      co_return;
    }
    if (slot.bytes == nullptr) {
      slot.bytes = new char[block_size];
    } else if (slot.dirty) {
      co_await device->write_block(slot.block_number, slot.bytes);
    }
    slot.block_number = block_number;
    slot.dirty = false;
    if (read) {
      co_await device->read_block(block_number, slot.bytes);
    }
  }

public:
  BlockCache(StrongRef<BlockIO> device, uint32_t n_slots)
      : BlockIO(device->block_size), device(device), n_slots(n_slots),
        slots(new Mutex<Slot>[n_slots]) {}

  // Frees the slots, none of which may be dirty.
  //   Q: Why can't the destructor write dirty blocks itself?
  //   Q: What should the owner do before dropping the last reference?
  virtual ~BlockCache() {
    for (uint32_t i = 0; i < n_slots; i++) {
      Slot &slot = slots[i].unshared();
      ASSERT(!slot.dirty);
      delete[] slot.bytes;
    }
    delete[] slots;
  }

  sync<uint32_t> size_in_bytes() override {
    co_return co_await device->size_in_bytes();
  }

  sync<void> read_block(uint32_t block_number, char *buffer) override {
    auto proof = co_await slots[block_number % n_slots];
    co_await load(proof, block_number, true);
    memcpy(buffer, proof.data().bytes, block_size);
  }

  sync<void> write_block(uint32_t block_number, const char *buffer) override {
    auto proof = co_await slots[block_number % n_slots];
    co_await load(proof, block_number, false);
    memcpy(proof.data().bytes, buffer, block_size);
    proof.data().dirty = true;
  }

  // Writes every dirty block to the device.
  //   Q: Can a slot become dirty again after flush has passed it?
  //   Q: Who calls it today?
  sync<void> flush() {
    for (uint32_t i = 0; i < n_slots; i++) {
      auto proof = co_await slots[i];
      Slot &slot = proof.data();
      if (slot.dirty) {
        co_await device->write_block(slot.block_number, slot.bytes);
        slot.dirty = false;
      }
    }
  }
};
