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

#include "mm/heap.h"

#include "arch/machine.h"
#include "lib/print.h"
#include "mm/physmem.h"
#include "mm/vmm.h"
#include "proc/ccb.h"
#include "sched/spin_lock.h"
#include <cstdint>
#include <new>

// The kernel heap: 32K slabs, each core allocating from its own.
//   Q: How does free find the slab a block came from?
//   Q: What happens to a block freed on a different core than its slab's?
//   Q: When does a slab that a core gave up become usable again?

namespace heap {

constexpr uint64_t SLAB = 32 * 1024;
constexpr uint64_t MAU = 16;
constexpr uint64_t MAUS = SLAB / MAU;
// Block sizes (in MAUs) that get their own free lists.
//   Q: What happens to a freed block bigger than this?
constexpr uint64_t SMALL = 64;
// Marks a free block in Slab::size.
//   Q: Why must MAUS be smaller than FREE?
constexpr uint16_t FREE = 0x8000;
static_assert(MAUS < FREE);
// Slab::outstanding's starting value while a core allocates from it.
//   Q: Why is it more than the slab could ever hand out?
constexpr int64_t ACTIVE = int64_t(MAUS);

// A free block, linked through its own first bytes.
//   Q: Why must it fit in one MAU?
struct FreeBlock {
  FreeBlock *next;
};
static_assert(sizeof(FreeBlock) <= MAU);

struct Slab {
  // Per MAU: the size of the block starting there (| FREE if free), or 0.
  //   Q: How does free use it to catch a double free?
  uint16_t size[MAUS]{};
  // The owner's free lists, by size.
  FreeBlock *free[SMALL + 1]{};
  // The owner's: the first MAU not handed out yet.
  uint64_t next = 0;
  // The owner's: blocks handed out minus blocks it got back.
  int64_t local = 0;
  // Blocks freed by other cores: a lock-free stack.
  //   Q: Why can anyone push, but only the owner pop?
  //   Q: Why does taking the whole stack at once avoid the ABA problem?
  FreeBlock *shared = nullptr;
  // Counts down to 0, when the slab is recycled.
  //   Q: What does it count while the slab is active, and after give_up?
  //   Q: Why is it updated atomically while local isn't?
  int64_t outstanding = ACTIVE;
  // The next slab in the pool, under lock.
  Slab *pool_next = nullptr;

  char *at(uint64_t mau) { return reinterpret_cast<char *>(this) + mau * MAU; }
  uint64_t mau_of(const void *p) const {
    return (uintptr_t(p) - uintptr_t(this)) / MAU;
  }
};

constexpr uint64_t META_MAUS = (sizeof(Slab) + MAU - 1) / MAU;
constexpr uint64_t SLAB_PAGES = SLAB / FRAME_SIZE;
static_assert(SLAB % FRAME_SIZE == 0);

} // namespace heap

namespace {

using heap::FREE;
using heap::FreeBlock;
using heap::MAU;
using heap::MAUS;
using heap::META_MAUS;
using heap::Slab;
using heap::SLAB;
using heap::SLAB_PAGES;
using heap::SMALL;

// The biggest block malloc hands out.
//   Q: Why can't a block span two slabs?
constexpr uint64_t MAX_BLOCK = (MAUS - META_MAUS) * MAU;

constexpr uint64_t REGION_SLABS = VMM::HEAP_BYTES / SLAB;

// The global lock.
//   Q: What does it protect, and why isn't it on malloc's fast path?
constinit SpinLock lock{};
// Healed slabs, ready for a core that needs one.
Slab *pool = nullptr;
// How many of the region's slabs are mapped so far.
//   Q: Does the heap ever unmap a slab?
uint64_t mapped = 0;

bool in_region(const void *p) {
  return uintptr_t(p) >= VMM::HEAP_START.va() &&
         uintptr_t(p) < VMM::HEAP_START.va() + VMM::HEAP_BYTES;
}

Slab *slab_of(const void *p) {
  return reinterpret_cast<Slab *>(uintptr_t(p) & ~(SLAB - 1));
}

// Returns a clean slab, from the pool or newly mapped.
//   Q: Why is the Slab constructed with placement new?
Slab *fresh() {
  void *where;
  {
    LockGuard g{lock};
    if (pool != nullptr) {
      where = pool;
      pool = pool->pool_next;
    } else {
      if (mapped == REGION_SLABS) {
        KPANIC("heap: the region is full (? bytes)\n", Dec(VMM::HEAP_BYTES));
      }
      const VPN first = VMM::HEAP_START.vpn() + mapped * SLAB_PAGES;
      mapped++;
      for (uint64_t page = 0; page < SLAB_PAGES; page++) {
        VMM::map_heap_page(first + page, physMem.alloc());
      }
      where = reinterpret_cast<void *>(VA{first}.va());
    }
  }
  Slab *const slab = new (where) Slab{};
  slab->next = META_MAUS;
  return slab;
}

// Puts a healed slab back in the pool.
void recycle(Slab *slab) {
  LockGuard g{lock};
  slab->pool_next = pool;
  pool = slab;
}

// Hands out n MAUs from the core's slab, or nullptr.
//   Q: Which does it try first: a free list or next?
void *take(Slab *slab, uint64_t n) {
  if (n <= SMALL && slab->free[n] != nullptr) {
    FreeBlock *const block = slab->free[n];
    slab->free[n] = block->next;
    slab->size[slab->mau_of(block)] = uint16_t(n);
    slab->local++;
    return block;
  }
  if (n > MAUS - slab->next) {
    return nullptr;
  }
  const uint64_t mau = slab->next;
  slab->next += n;
  slab->size[mau] = uint16_t(n);
  slab->local++;
  return slab->at(mau);
}

// Takes back a free block of n MAUs, in the owner's slab.
//   Q: What happens to a big block that isn't the last one handed out?
void put(Slab *slab, FreeBlock *block, uint64_t n) {
  if (n <= SMALL) {
    block->next = slab->free[n];
    slab->free[n] = block;
    return;
  }
  const uint64_t mau = slab->mau_of(block);
  if (mau + n == slab->next) {
    slab->size[mau] = 0;
    slab->next = mau;
  }
}

// The owner takes back every block on its slab's shared list.
//   Q: Why does outstanding go up when local goes down?
void collect(Slab *slab) {
  FreeBlock *block =
      __atomic_exchange_n(&slab->shared, nullptr, __ATOMIC_ACQUIRE);
  int64_t taken = 0;
  while (block != nullptr) {
    FreeBlock *const next = block->next;
    put(slab, block, slab->size[slab->mau_of(block)] & ~FREE);
    taken++;
    block = next;
  }
  if (taken != 0) {
    slab->local -= taken;
    __atomic_add_fetch(&slab->outstanding, taken, __ATOMIC_RELAXED);
  }
}

// The owner gives its slab up for a fresh one.
//   Q: How does outstanding change meaning here?
//   Q: Who recycles the slab if blocks are still out?
void give_up(Slab *slab) {
  if (__atomic_add_fetch(&slab->outstanding, slab->local - heap::ACTIVE,
                         __ATOMIC_ACQ_REL) == 0) {
    recycle(slab);
  }
}

// Hands out n MAUs from the core's active slab, replacing it if full.
//   Q: In what order does it try take, collect and fresh?
void *alloc(Slab *&active, uint64_t n) {
  if (active != nullptr) {
    if (void *p = take(active, n)) {
      return p;
    }
    collect(active);
    if (void *p = take(active, n)) {
      return p;
    }
    give_up(active);
  }
  active = fresh();
  return take(active, n);
}

} // namespace

void heap::init_system() { VMM::init_heap_region(); }

void *malloc(std::size_t bytes) noexcept {
  if (bytes > MAX_BLOCK) {
    KPANIC("malloc(?): bigger than a slab can hold (?)\n", Dec(bytes),
           Dec(MAX_BLOCK));
  }
  const uint64_t n = bytes == 0 ? 1 : (bytes + MAU - 1) / MAU;
  // Interrupts off: nothing else on this core touches its active slab.
  const bool was = disable();
  void *p = alloc(CCB::current()->slab, n);
  restore(was);
  return p;
}

void free(void *p) noexcept {
  if (p == nullptr) {
    return;
  }
  ASSERT(in_region(p) && uintptr_t(p) % MAU == 0);
  Slab *const slab = slab_of(p);
  const uint64_t mau = slab->mau_of(p);
  ASSERT(mau >= META_MAUS);
  const uint16_t n = slab->size[mau];
  if (n == 0 || (n & FREE) != 0) {
    KPANIC("free(?): not an allocated block (size ?)\n", p, n);
  }
  slab->size[mau] = n | FREE;
  FreeBlock *const block = static_cast<FreeBlock *>(p);

  const bool was = disable();
  if (CCB::current()->slab == slab) {
    // this core's: no lock
    put(slab, block, n);
    slab->local--;
    restore(was);
    return;
  }
  restore(was);

  // Another core's slab, or one given up: push on its shared list.
  FreeBlock *old = __atomic_load_n(&slab->shared, __ATOMIC_RELAXED);
  do {
    block->next = old;
  } while (!__atomic_compare_exchange_n(&slab->shared, &old, block, true,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  if (__atomic_sub_fetch(&slab->outstanding, 1, __ATOMIC_ACQ_REL) == 0) {
    recycle(slab);
  }
}

/*****************/
/* C++ operators */
/*****************/

void *operator new(std::size_t size) { return malloc(size); }

void operator delete(void *p) noexcept { return free(p); }

void operator delete(void *p, std::size_t) noexcept { return free(p); }

void *operator new[](std::size_t size) { return malloc(size); }

void operator delete[](void *p) noexcept { return free(p); }

void operator delete[](void *p, std::size_t) noexcept { return free(p); }
