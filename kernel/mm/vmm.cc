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

#include "mm/vmm.h"

#include "arch/idt.h"
#include "arch/machine.h"
#include "fs/ext2.h"
#include "lib/errno.h"
#include "lib/print.h"
#include "lib/shared.h"
#include "mm/tlb.h"
#include "proc/ccb.h"
#include "proc/pcb.h"
#include "proc/tcb.h"
#include "proc/signals.h"
#include "sched/mutex.h"
#include <cstdint>
#include <expected>
#include <utility>

namespace impl {

constexpr VPN vme_start{VA_START};

constexpr VPN vme_end{VA_END};

constexpr uint64_t max_pages = vme_end.vpn() - vme_start.vpn();

// File offsets are kept as VPNs, so page numbers must stay below this.
constexpr uint64_t vpn_limit = UINT64_C(1) << (48 - LOG_FRAME_SIZE);

// What a mapping lets user code do with its pages.
//   Q: Why is there no WRITE-only access?
//   Q: What happens to PROT_EXEC?
enum class Access { NONE, READ, READ_WRITE };

static Access access_for(int prot) {
  if ((prot & VMM::PROT_WRITE) != 0) {
    return Access::READ_WRITE;
  }
  if ((prot & (VMM::PROT_READ | VMM::PROT_EXEC)) != 0) {
    return Access::READ;
  }
  return Access::NONE;
}

// Does access allow a read (or, if write, a write)?
static bool allows(Access access, bool write) {
  return write ? access == Access::READ_WRITE : access != Access::NONE;
}

// mmap's arguments, checked and converted: the file (nullptr for anonymous
// memory), where in it the mapping starts, and what user code may do with its
// pages. Each VME keeps the ones that made it.
struct MmapArgs {
  const StrongRef<Node> file;
  const VPN file_offset;
  Access access;
  bool executable;
  StrongRef<CachedPages> shared{};

  // The same arguments, n pages further in (for the second half of a split).
  //   Q: Why does an anonymous mapping's offset not matter?
  MmapArgs skip(VPN n) const { return {file, file_offset + n, access, executable, shared}; }
};

// A reserved range of virtual pages [start, end) and what it holds.
//   Q: Why does the list start with a dummy VME covering page 0?
//   Q: Which lock protects the list?
struct VME {
  VPN start;
  VPN end;
  MmapArgs args;
  VME *next;

  VME() = delete;
  VME(VPN start, VPN end, const MmapArgs &args, VME *next)
      : start(start), end(end), args(args), next(next) {}

  bool contains(VPN vpn) const { return vpn >= start && vpn < end; }

  // Returns the VME containing vpn, or nullptr.
  //   Q: Why does it need the Proof, and what does it never return?
  static VME *find(VPN vpn, Mutex<VME *>::Proof &proof);

  // Finds a big enough gap in the VME list and inserts a new VME there.
  //   Q: Which gap does it pick when several fit?
  static VME *gap(VPN n_pages, const MmapArgs &args,
                  Mutex<VME *>::Proof &proof);
};

VME *VME::find(VPN vpn, Mutex<VME *>::Proof &proof) {
  if (vpn < vme_start || vpn >= vme_end) {
    return nullptr;
  }
  VME *p = proof.data()->next; // skip the low-mem dummy
  while (p != nullptr) {
    if (p->contains(vpn))
      return p;
    p = p->next;
  }
  return nullptr;
}

VME *VME::gap(VPN n_pages, const MmapArgs &args, Mutex<VME *>::Proof &proof) {
  VME *prev = proof.data(); // first entry is always there, representing
                            // unmappable low-mem
  VME *p = prev->next;
  VPN candidate = prev->end;
  while (p != nullptr) {
    if (candidate + n_pages <= p->start)
      break;

    candidate = p->end;
    prev = p;
    p = p->next;
  }
  // past the last VME, everything up to vme_end is free
  if (p == nullptr && candidate + n_pages > vme_end) {
    return nullptr;
  }

  VME *vme = new VME(candidate, candidate + n_pages, args, p);

  ASSERT(prev != nullptr);
  prev->next = vme;

  return vme;
}

struct PTE {
  union {
    uint64_t val;
    struct {
      uint64_t present : 1;
      uint64_t write : 1;
      uint64_t user : 1;
      uint64_t pwt : 1;
      uint64_t pcd : 1;
      uint64_t accessed : 1;
      uint64_t dirty : 1;
      uint64_t pat : 1;
      uint64_t global : 1;
      uint64_t ignored : 3;
      uint64_t addr : 40;
      uint64_t reserved : 11;
      uint64_t xd : 1;
    };
  };

  PTE(uint64_t v) : val(v) {}
};

struct CR3 {
  union {
    uint64_t val;
    struct {
      uint64_t : 12;
      uint64_t addr : 40;
      uint64_t : 12;
    };
  };

  CR3(uint64_t v) : val(v) {}
};

// Page tables: low-mem per address space, high-mem shared by all.
//   Q: Which lock protects an address space's low-mem tables?
//   Q: Why can every PML4 point to the same high-mem tables?

// Writes a page-table entry with one store.
//   Q: Who reads entries without taking any lock?
//   Q: Why release ordering?
static void set_pte(PTE *pte, PTE value) {
  __atomic_store_n(&pte->val, value.val, __ATOMIC_RELEASE);
}

// A present entry for ppn.
static PTE make_pte(PPN ppn, bool write, bool user, bool xd) {
  PTE e{0};
  e.present = 1;
  e.write = write ? 1 : 0;
  e.user = user ? 1 : 0;
  e.xd = xd ? 1 : 0;
  e.addr = ppn.ppn();
  return e;
}

// Returns the leaf PTE for vpn, creating missing tables if create.
//   Q: What must the caller hold?
//   Q: Why does it refuse to create a high-mem PML4 entry?
static PTE *walk(CR3 cr3, VPN vpn, bool create) {
  PTE *table = (PTE *)VA{PPN{cr3.addr}}.va();
  PTE *pte = nullptr;
  for (int level = 3; level > 0; level--) {
    pte = &table[(vpn.vpn() >> (9 * level)) & 0x1FF];
    if ((pte->present) == 0) {
      if (!create) {
        return nullptr;
      }
      // No new high-mem PML4 entries after boot: old address spaces lack them.
      if (level == 3 && (vpn.vpn() >> 35) != 0) {
        KPANIC(
            "new high-mem PML4 entry for ?, address spaces wouldn't see it\n",
            vpn);
      }
      // A fresh zeroed table; leaf entries decide the real permissions.
      set_pte(pte, make_pte(physMem.alloc(), true, true, false));
    }
    table = (PTE *)VA{PPN{pte->addr}}.va();
  }
  // table is now the page table, pick the leaf entry
  return &table[vpn.vpn() & 0x1FF];
}

static void map(CR3 cr3, VPN vpn, PPN ppn, bool user, bool write, bool xd = false) {
  PTE *pte = walk(cr3, vpn, true);
  ASSERT(pte != nullptr);
  ASSERT(pte->present == 0);
  set_pte(pte, make_pte(ppn, write, user, xd));
}

// Software bit 9 identifies writable private pages shared by fork.
constexpr uint64_t COW = uint64_t(1) << 9;
constexpr uint64_t SHARED = uint64_t(1) << 10;

static sync<void> make_private(CR3 cr3, VPN vpn, PTE *pte) {
  PPN old{pte->addr};
  PTE entry = *pte;
  if (physMem.owners(old) > 1) {
    PPN fresh = physMem.alloc();
    memcpy((void *)VA{fresh}.va(), (void *)VA{old}.va(), FRAME_SIZE);
    entry.addr = fresh.ppn();
  }
  entry.write = 1;
  entry.val &= ~COW;
  set_pte(pte, entry);
  co_await TLB::Shootdown{vpn, vpn + 1};
  if (entry.addr != old.ppn()) physMem.free(old);
}

// Frames waiting for a TLB shootdown before they can be freed.
//   Q: Why aren't they linked through their own memory?
class FrameList {
  struct Chunk {
    static constexpr uint64_t N = FRAME_SIZE / sizeof(uint64_t) - 2;
    Chunk *next;
    uint64_t count;
    uint64_t ppns[N];
  };
  Chunk *head = nullptr;

public:
  FrameList() = default;
  FrameList(const FrameList &) = delete;
  FrameList &operator=(const FrameList &) = delete;
  ~FrameList() { ASSERT(head == nullptr); }

  bool empty() const { return head == nullptr; }

  void add(PPN ppn) {
    if (head == nullptr || head->count == Chunk::N) {
      head = new Chunk{head, 0, {}};
    }
    head->ppns[head->count++] = ppn.ppn();
  }

  void free_all() {
    while (head != nullptr) {
      for (uint64_t i = 0; i < head->count; i++) {
        physMem.free(PPN{head->ppns[i]});
      }
      Chunk *next = head->next;
      delete head;
      head = next;
    }
  }
};

// Unmaps every page in [lo, hi), adding the frames to freed.
//   Q: Why doesn't it flush the TLB itself?
//   Q: How does it skip regions without page tables?
//   Q: What is [flush_lo, flush_hi) for?
static void unmap_range(CR3 cr3, VPN lo, VPN hi, FrameList &freed,
                        VPN &flush_lo, VPN &flush_hi) {
  // raw page numbers, VPN masks its value and a skip past the end could wrap
  uint64_t p = lo.vpn();
  const uint64_t end = hi.vpn();

  while (p < end) {
    PTE *table = (PTE *)VA{PPN{cr3.addr}}.va();
    int missing = 0; // level of the first missing table entry, 0 if none
    for (int level = 3; level > 0; level--) {
      PTE *pte = &table[(p >> (9 * level)) & 0x1FF];
      if ((pte->present) == 0) {
        missing = level;
        break;
      }
      table = (PTE *)VA{PPN{pte->addr}}.va();
    }

    if (missing != 0) {
      // nothing is mapped until the next region covered by that entry
      const uint64_t span = uint64_t(1) << (9 * missing);
      p = (p / span + 1) * span;
      continue;
    }

    PTE *pte = &table[p & 0x1FF];
    if ((pte->present) != 0) {
      freed.add(PPN{pte->addr});
      set_pte(pte, PTE{0});
      if (VPN{p} < flush_lo)
        flush_lo = VPN{p};
      flush_hi = VPN{p + 1};
    }
    p++;
  }
}

// The number of pages that hold length bytes.
//   Q: Why can't it overflow when rounding length up first could?
static constexpr uint64_t pages_for(uint64_t length) {
  return length / FRAME_SIZE + ((length % FRAME_SIZE) != 0 ? 1 : 0);
}

// Splits vme at vpn: vme keeps [start, vpn), a new VME gets the rest.
static void split(VME *vme, VPN vpn, Mutex<VME *>::Proof &) {
  vme->next =
      new VME(vpn, vme->end, vme->args.skip(vpn - vme->start), vme->next);
  vme->end = vpn;
}

// Unmaps [first, last), putting a VME for args there (MAP_FIXED).
//   Q: Why does it hold the lock through the TLB shootdown?
//   Q: What does a fault in the range see while it runs, and after?
//   Q: Why send the shootdown to every core?
static sync<void> unmap_pages(Process process, VPN first, VPN last,
                              const MmapArgs *args) {
  const CR3 cr3{process->cr3};
  // frames go back to physMem only after every core's TLB forgot them
  FrameList freed;
  // The pages actually mapped: the only ones a TLB can hold.
  VPN flush_lo = last;
  VPN flush_hi = first;

  auto proof = co_await process->vmes;
  // One pass over the VMEs; prev ends as the last one before the range.
  VME *prev = proof.data();
  while (prev->next != nullptr && prev->next->start < last) {
    VME *const vme = prev->next;
    if (vme->end <= first) {
      // before the range
      prev = vme;
      continue;
    }
    if (vme->start < first) {
      // its head stays, the rest is next
      split(vme, first, proof);
      prev = vme;
      continue;
    }
    if (vme->end > last) {
      // its tail stays
      split(vme, last, proof);
    }
    // in the range: its pages, then itself
    unmap_range(cr3, vme->start, vme->end, freed, flush_lo, flush_hi);
    prev->next = vme->next;
    delete vme;
  }

  if (!freed.empty()) {
    co_await TLB::Shootdown{flush_lo, flush_hi};
    freed.free_all();
  }

  if (args != nullptr) {
    prev->next = new VME(first, last, *args, prev->next);
  }
}

// A range of pages, [first, last).
struct Pages {
  VPN first;
  VPN last;
};

// A MAP_FIXED range as pages, or Linux's error: ENOMEM past the end of user
// memory, EINVAL unaligned, EPERM below the lowest address mmap gives
// (Linux's mmap_min_addr), checked in that order. length is checked already.
//   Q: Why check the raw values before making VPNs?
static std::expected<Pages, int64_t> fixed_pages(void *addr,
                                                 std::size_t length) {
  const uint64_t va = uint64_t(addr);
  if (va > VA_END.va() - length) {
    return std::unexpected(Errno::ENOMEM);
  }
  if ((va & (FRAME_SIZE - 1)) != 0) {
    return std::unexpected(Errno::EINVAL);
  }
  if (va < VA_START.va()) {
    return std::unexpected(Errno::EPERM);
  }
  const VPN first{VA{va}};
  return Pages{first, first + pages_for(length)};
}

// munmap's range as pages, or nothing if it isn't aligned, is empty, or
// isn't low-mem.
//   Q: Why is page 0 allowed here but not for MAP_FIXED?
static std::optional<Pages> munmap_pages(void *addr, std::size_t length) {
  // Check raw values before making VPNs; page 0 is fine here.
  const VA va{uint64_t(addr)};
  if (length == 0 || (va.va() & (FRAME_SIZE - 1)) != 0) {
    return std::nullopt;
  }
  if (va >= VA_END) {
    return std::nullopt;
  }
  const VPN first{va};
  // the whole range has to be in low-mem
  if (pages_for(length) > vme_end.vpn() - first.vpn()) {
    return std::nullopt;
  }
  return Pages{first, first + pages_for(length)};
}

// Checks mmap's length (and a file's offset) and converts the arguments, or
// Linux's error: do_mmap's first checks.
//   Q: Why is an oversized length ENOMEM but a zero one EINVAL?
//   Q: Which prot bits are ignored?
static std::expected<MmapArgs, int64_t>
parse_mmap_args(std::size_t length, int prot, const StrongRef<Node> &file,
                uint64_t offset) {
  if (length == 0) {
    return std::unexpected(Errno::EINVAL);
  }
  // Rounded up to pages, it wraps around or is more than user memory.
  if (length > VA_END.va()) {
    return std::unexpected(Errno::ENOMEM);
  }
  // File pages must fit in a VPN, like Linux's EOVERFLOW check (anonymous
  // mappings ignore offset).
  if (file != nullptr &&
      (offset >> LOG_FRAME_SIZE) + pages_for(length) > vpn_limit) {
    return std::unexpected(Errno::EOVERFLOW);
  }
  // prot: bits other than READ, WRITE and EXEC are ignored, like Linux
  return MmapArgs{file, VPN{VA{offset}}, access_for(prot), (prot & VMM::PROT_EXEC) != 0};
}

// The mapping type, which Linux checks after the address: 0 if fine, or
// EINVAL. Other flags are ignored.
//   Q: What does MAP_SHARED | MAP_PRIVATE (MAP_SHARED_VALIDATE) mean, and
//      why not for anonymous memory?
static int64_t check_map_type(int flags, const StrongRef<Node> &file) {
  using VMM::MAP_ANONYMOUS, VMM::MAP_PRIVATE, VMM::MAP_SHARED, VMM::MAP_TYPE;
  if (((flags & MAP_ANONYMOUS) != 0) != (file == nullptr)) {
    return Errno::EINVAL;
  }
  const int type = flags & MAP_TYPE;
  // Both file-backed and anonymous mappings support shared and private pages.
  if (type == MAP_PRIVATE || type == MAP_SHARED) {
    return 0;
  }
  return Errno::EINVAL;
}

static PTE *table_of(PPN ppn) { return (PTE *)VA{ppn}.va(); }

// PML4 slots [256, 512) are high-mem.
constexpr uint64_t FIRST_UPPER_SLOT = 256;

// Frees the low-mem page tables under pml4 (leaf entries already clear).
static void free_low_tables(PTE *pml4) {
  for (uint64_t i = 0; i < FIRST_UPPER_SLOT; i++) {
    if (pml4[i].present == 0) {
      continue;
    }
    PTE *pdpt = table_of(PPN{pml4[i].addr});
    for (uint64_t j = 0; j < 512; j++) {
      if (pdpt[j].present == 0) {
        continue;
      }
      PTE *pd = table_of(PPN{pdpt[j].addr});
      for (uint64_t k = 0; k < 512; k++) {
        if (pd[k].present != 0) {
          physMem.free(PPN{pd[k].addr});
        }
      }
      physMem.free(PPN{pdpt[j].addr});
    }
    physMem.free(PPN{pml4[i].addr});
  }
}

// One PML4 entry covers 512G.
static_assert(((VMM::HEAP_START.va() >> 39) & 0x1FF) ==
              (((VMM::HEAP_START.va() + VMM::HEAP_BYTES - 1) >> 39) & 0x1FF));

static bool in_heap_region(VPN vpn) {
  return vpn >= VMM::HEAP_START.vpn() &&
         vpn < VMM::HEAP_START.vpn() + (VMM::HEAP_BYTES >> LOG_FRAME_SIZE);
}

// The kernel's CR3, for the heap region.
//   Q: Why can't the heap use Process::kernel()'s CR3?
static uint64_t heap_cr3 = 0;

// The byte offset in vme's file that backs vpn.
static uint64_t file_byte_for(const VME *vme, VPN vpn) {
  return (vme->args.file_offset + vpn - vme->start).vpn() * FRAME_SIZE;
}

// Untouched anonymous pages share one immutable zero frame until a write.
static SpinLock zero_lock{};
static uint64_t zero_frame = 0;
static PPN shared_zero() {
  LockGuard guard{zero_lock};
  if (!zero_frame) zero_frame = physMem.alloc().ppn();
  PPN frame{zero_frame};
  physMem.retain(frame);
  return frame;
}

// Maps va's page if a VME allows the access; false for a segfault.
//   Q: What three cases make it return false?
//   Q: Why does it hold the lock while reading a file page?
//   Q: What if another thread already mapped the page?
static sync<bool> resolve_fault(Process process, VA va, bool write) {

  // va may be high-mem: make the VPN only after the low-mem check.
  const VPN vpn{va};
  const CR3 cr3{process->cr3};

  auto proof = co_await process->vmes;
  const VME *vme = (va < VA_END) ? VME::find(vpn, proof) : nullptr;
  if (vme == nullptr || !allows(vme->args.access, write)) {
    co_return false;
  }
  PTE *existing = walk(cr3, vpn, false);
  if (existing && existing->present) {
    if (write && (existing->val & COW)) co_await make_private(cr3, vpn, existing);
    co_return !write || existing->write;
  }

  if (vme->args.shared != nullptr) {
    uint64_t index = (vme->args.file_offset + vpn - vme->start).vpn();
    if (vme->args.file != nullptr && index * FRAME_SIZE >=
        co_await vme->args.file->size_in_bytes()) co_return false;
    PPN frame{co_await vme->args.shared->get(index, vme->args.file != nullptr ? vme->args.file.operator->() : nullptr)};
    physMem.retain(frame);
    auto entry = make_pte(frame, vme->args.access == Access::READ_WRITE,
                          true, !vme->args.executable);
    entry.val |= SHARED;
    set_pte(walk(cr3, vpn, true), entry);
    co_return true;
  }
  if (!write && vme->args.file == nullptr) {
    // Fault in a small run of zero-page translations together. Large sparse
    // reservations stay sparse, while sequential reads avoid repeated traps.
    VPN first{vpn.vpn() & ~uint64_t(63)};
    if (first < vme->start) first = vme->start;
    VPN end = first + 64;
    if (end > vme->end) end = vme->end;
    for (VPN page = first; page < end; page = page + 1) {
      PTE *pte = walk(cr3, page, true);
      if (pte->present) continue;
      PTE entry = make_pte(shared_zero(), false, true, !vme->args.executable);
      if (vme->args.access == Access::READ_WRITE) entry.val |= COW;
      set_pte(pte, entry);
    }
    co_return true;
  }
  const PPN frame = physMem.alloc();
  if (vme->args.file != nullptr) {
    // the file's page, zeros past its end
    const uint64_t file_byte = file_byte_for(vme, vpn);
    if (file_byte < co_await vme->args.file->size_in_bytes()) {
      const auto n = co_await vme->args.file->read_all(uint32_t(file_byte),
                                                       FRAME_SIZE, VA{frame});
      ASSERT(n > 0);
    }
  }
  // low-mem is user memory
  map(cr3, vpn, frame, true, vme->args.access == Access::READ_WRITE, !vme->args.executable);
  co_return true;
}

////////////////////////////// Pinning //////////////////////////////

// Is [va, va + n) low-mem (user memory)?
static bool is_user_range(VA va, std::size_t n) {
  const uint64_t start = va.va();
  return start >= VA_START.va() && start < VA_END.va() &&
         n <= VA_END.va() - start;
}

// How much of a copy fits on at's page.
static uint64_t piece(VA at, uint64_t left) {
  const uint64_t room = FRAME_SIZE - at.va() % FRAME_SIZE;
  return left < room ? left : room;
}

// What one try could do (see try_with_user_page).
enum class PageTry {
  DONE,       // f ran
  NOT_MAPPED, // its fault has to happen first (resolve_fault)
  REFUSED,    // user code couldn't: not mapped, or no access
};

// Runs f on one page's bytes in place, if user code could access them.
//   Q: Why must it hold the address space's lock?
//   Q: What does it return when the page isn't mapped yet?
static PageTry try_with_user_page(Mutex<VME *>::Proof &proof, CR3 cr3, VA at,
                                  uint64_t count, bool write,
                                  const VMM::PageBytes &f, uint64_t &used) {
  const VME *vme = VME::find(VPN{at}, proof);
  if (vme == nullptr || !allows(vme->args.access, write)) {
    return PageTry::REFUSED;
  }
  const PTE *pte = walk(cr3, VPN{at}, false);
  if (pte == nullptr || pte->present == 0 || (write && (pte->val & COW))) {
    return PageTry::NOT_MAPPED;
  }
  used = f((char *)VA{PPN{pte->addr}} + at.va() % FRAME_SIZE, 0, count);
  return PageTry::DONE;
}

////////////////////////////// Page faults //////////////////////////////

// Ends the whole process, like SIGSEGV.
//   Q: Why must exit_group come before the thread's exit?
static sync<void> segmentation_violation(Thread thread, VA va) {
  int code = 1; // SEGV_MAPERR: no mapping covers the address.
  {
    auto proof = co_await thread->process->vmes;
    if (va >= VA_START && va < VA_END && VME::find(VPN{va}, proof))
      code = 2; // SEGV_ACCERR: a mapping exists but rejects the access.
  }
  Signals::fault(std::move(thread), va.va(), code);
}

// Handles a user page fault, start to finish.
//   Q: What are the three ways it can end?
//   Q: Why check exiting before resolving the fault?
static sync<void> handle_user_fault(Thread thread, VA va, uint64_t error_code) {
  if ((error_code & 1) != 0 && (error_code & 2) == 0) {
    // A present page means a protection violation (e.g. kernel memory)
    co_await segmentation_violation(std::move(thread), va);
    co_return;
  }
  if (thread->process->exiting()) {
    // exit_group from another thread: this one ends instead
    std::move(thread).exit(0);
    co_return;
  }
  // error code bit 1: it was a write
  if (co_await resolve_fault(thread->process, va, (error_code & 2) != 0)) {
    // A resolved demand/COW fault retries the interrupted instruction. It is
    // not a signal delivery point: a nested handler here can overwrite an
    // in-progress read/modify/write in the interrupted handler.
    thread->signal_checked = true;
    std::move(thread).schedule();
  } else {
    co_await segmentation_violation(std::move(thread), va);
  }
}

// Starts the fault's coroutine without waiting for it.
//   Q: Why must this be its own noinline function? (see Syscall::start)
[[gnu::noinline]] static void start_user_fault(VA va, uint64_t error_code) {
  (void)handle_user_fault(Thread::current(), va, error_code);
}

} // namespace impl

uint64_t VA::check_canonical(uint64_t va) {
  int64_t sa = int64_t(va);
  ASSERT(((sa << 16) >> 16) == sa);
  return va;
}

sync<std::expected<void *, int64_t>> VMM::mmap(Process process, void *addr,
                                               std::size_t length, int prot,
                                               int flags, StrongRef<Node> file,
                                               uint64_t offset) {
  using namespace impl;

  std::expected<MmapArgs, int64_t> args =
      parse_mmap_args(length, prot, file, offset);
  if (!args) {
    co_return std::unexpected(args.error());
  }

  if ((flags & MAP_TYPE) == MAP_SHARED) {
    args->shared = file != nullptr ? file->cache : StrongRef<CachedPages>::make();
  }

  if ((flags & MAP_FIXED) != 0) {
    const std::expected<Pages, int64_t> pages = fixed_pages(addr, length);
    if (!pages) {
      co_return std::unexpected(pages.error());
    }
    if (const int64_t error = check_map_type(flags, file); error != 0) {
      co_return std::unexpected(error);
    }
    co_await unmap_pages(process, pages->first, pages->last, &*args);
    co_return addr;
  }

  // (Linux would find the room first: no room and a bad type is ENOMEM)
  if (const int64_t error = check_map_type(flags, file); error != 0) {
    co_return std::unexpected(error);
  }
  // addr is only a hint, ignored
  auto proof = co_await process->vmes;
  auto vme = VME::gap(VPN{pages_for(length)}, *args, proof);
  if (vme == nullptr) {
    co_return std::unexpected(Errno::ENOMEM);
  }
  co_return (void *) VA{vme->start}.va();
}

sync<int> VMM::munmap(Process process, void *addr, std::size_t length) {
  using namespace impl;

  const std::optional<Pages> pages = munmap_pages(addr, length);
  if (!pages) {
    co_return -1;
  }
  co_await unmap_pages(process, pages->first, pages->last, nullptr);
  co_return 0;
}

////////////////////////////// Address spaces //////////////////////////////

impl::VME *VMM::new_vmes() {
  using namespace impl;
  return new VME{VPN{0}, vme_start, MmapArgs{{}, VPN{0}, Access::NONE, false},
                 nullptr};
}

uint64_t VMM::new_page_tables() {
  using namespace impl;

  // Low-mem empty, high-mem shared with the kernel.
  const PPN pml4 = physMem.alloc();
  PTE *to = table_of(pml4);
  const PTE *from = table_of(PPN{CR3{Process::kernel()->cr3}.addr});
  for (uint64_t slot = FIRST_UPPER_SLOT; slot < 512; slot++) {
    to[slot] = from[slot];
  }
  return PA{pml4}.pa();
}

namespace {
// Walk each populated page-table page once, rather than walking four levels
// again for each resident user page.
void fork_table(impl::PTE *source, impl::PTE *destination, int level, unsigned count) {
  using namespace impl;
  for (unsigned i = 0; i < count; ++i) {
    if (!source[i].present) continue;
    PTE entry = source[i];
    if (level) {
      PPN table = physMem.alloc();
      entry.addr = table.ppn();
      set_pte(&destination[i], entry);
      fork_table(table_of(PPN{source[i].addr}), table_of(table), level - 1, 512);
    } else {
      if (entry.write && !(entry.val & SHARED)) {
        entry.write = 0;
        entry.val |= COW;
        set_pte(&source[i], entry);
      }
      physMem.retain(PPN{entry.addr});
      set_pte(&destination[i], entry);
    }
  }
}
}

sync<void> VMM::fork_memory(Process parent, Process child) {
  using namespace impl;
  auto src = co_await parent->vmes;
  auto dst = co_await child->vmes;
  VME *tail = dst.data();
  for (VME *v = src.data()->next; v; v = v->next) {
    tail->next = new VME(v->start, v->end, v->args, nullptr);
    tail = tail->next;
  }
  fork_table(table_of(PPN{CR3{parent->cr3}.addr}),
             table_of(PPN{CR3{child->cr3}.addr}), 3, FIRST_UPPER_SLOT);
  // No parent thread may retain a writable translation to a shared frame.
  co_await TLB::Shootdown{vme_start, vme_end};
}

sync<void> VMM::protect(Process process, uint64_t start, uint64_t length, int prot) {
  using namespace impl;
  if (!length) co_return;
  auto proof = co_await process->vmes;
  VPN first{VA{start}}, last = first + pages_for(length);
  for (VME *v = proof.data()->next; v; v = v->next) {
    if (v->end <= first || v->start >= last) continue;
    if (v->start < first) { split(v, first, proof); v = v->next; }
    if (v->end > last) split(v, last, proof);
    v->args.access = access_for(prot);
    v->args.executable = (prot & PROT_EXEC) != 0;
    for (VPN page = v->start; page < v->end; page = page + 1) {
      PTE *pte = walk(CR3{process->cr3}, page, false);
      if (pte && pte->present) {
        PTE entry = *pte;
        entry.write = (prot & PROT_WRITE) != 0;
        entry.xd = (prot & PROT_EXEC) == 0;
        set_pte(pte, entry);
      }
    }
  }
  co_await TLB::Shootdown{first, last};
}

sync<void> VMM::replace_memory(Process target, Process source) {
  using namespace impl;
  auto a = co_await target->vmes;
  auto b = co_await source->vmes;
  auto old = a.data(); a.data() = b.data(); b.data() = old;
  PTE *target_root = table_of(PPN{CR3{target->cr3}.addr});
  PTE *source_root = table_of(PPN{CR3{source->cr3}.addr});
  for (uint64_t i=0;i<FIRST_UPPER_SLOT;++i) {
    PTE entry=target_root[i];
    set_pte(&target_root[i],source_root[i]);
    set_pte(&source_root[i],entry);
  }
  // Retain the process's root on every core, then retire its old tables.
  co_await TLB::Shootdown{vme_start,vme_end};
}

void VMM::free_memory(uint64_t cr3_value, Mutex<impl::VME *> &vmes) {
  using namespace impl;
  const CR3 cr3{cr3_value};

  // Nobody can use it any more: pages, then tables, then VMEs.
  FrameList freed;
  VPN flush_lo = vme_end;
  VPN flush_hi = vme_start;
  unmap_range(cr3, vme_start, vme_end, freed, flush_lo, flush_hi);
  freed.free_all();

  PTE *pml4 = table_of(PPN{cr3.addr});
  free_low_tables(pml4);
  physMem.free(PPN{cr3.addr});

  VME *vme = vmes.unshared();
  while (vme != nullptr) {
    VME *const next = vme->next;
    delete vme;
    vme = next;
  }
}

void VMM::init_heap_region() {
  using namespace impl;
  heap_cr3 = get_cr3();
  PTE *pml4 = table_of(PPN{CR3{heap_cr3}.addr});
  PTE *entry = &pml4[(HEAP_START.va() >> 39) & 0x1FF];
  ASSERT(entry->present == 0);
  set_pte(entry, make_pte(physMem.alloc(), true, false, false));
}

void VMM::map_heap_page(VPN vpn, PPN ppn) {
  using namespace impl;
  ASSERT(in_heap_region(vpn));
  map(CR3{heap_cr3}, vpn, ppn, false, true);
}

VA VMM::map_device(PA pa, uint64_t bytes) {
  using namespace impl;
  ASSERT(pa.pa() % FRAME_SIZE == 0 && bytes % FRAME_SIZE == 0);

  // the next unused byte of the region
  static SpinLock lock{};
  static uint64_t used = 0;
  LockGuard g{lock};

  ASSERT(bytes <= DEVICE_BYTES - used);
  const VA start{DEVICE_START.va() + used};
  used += bytes;
  for (uint64_t i = 0; i < bytes; i += FRAME_SIZE) {
    map(CR3{heap_cr3}, VPN{VA{start.va() + i}}, PPN{PA{pa.pa() + i}}, false,
        true);
  }
  return start;
}

////////////////////////////// Pinning //////////////////////////////

sync<std::optional<uint64_t>> VMM::with_user_page(Process process, VA va,
                                                  std::size_t n, bool write,
                                                  PageBytes f) {
  using namespace impl;
  if (!is_user_range(va, n)) {
    co_return std::nullopt;
  }
  const CR3 cr3{process->cr3};
  const uint64_t count = piece(va, n);
  while (true) {
    PageTry result;
    uint64_t used = 0;
    {
      auto proof = co_await process->vmes;
      result = try_with_user_page(proof, cr3, va, count, write, f, used);
    }
    switch (result) {
    case PageTry::DONE:
      co_return used;
    case PageTry::REFUSED:
      co_return std::nullopt;
    case PageTry::NOT_MAPPED:
      // fault it in, then the page again
      if (!co_await resolve_fault(process, va, write)) {
        co_return std::nullopt;
      }
      break;
    }
  }
}

sync<bool> VMM::with_user_pages(Process process, VA va, std::size_t n,
                                bool write, PageBytes f) {
  uint64_t done = 0;
  while (done < n) {
    const std::optional<uint64_t> used = co_await with_user_page(
        process, VA{va.va() + done}, n - done, write,
        [&f, &done](char *bytes, uint64_t, uint64_t count) {
          return f(bytes, done, count);
        });
    // a bad address, or f took nothing (it would never get further)
    if (!used || *used == 0) {
      co_return false;
    }
    done += *used;
  }
  co_return true;
}

namespace {
sync<bool> copy_user(Process process, VA address, void *kernel, std::size_t n, bool write) {
  using namespace impl;
  if (!n) co_return true;
  if (!is_user_range(address, n)) co_return false;
  uint64_t done = 0;
  while (done < n) {
    VA at{address.va() + done};
    uint64_t count = piece(at, n - done), used = 0;
    PageTry result;
    {
      auto proof = co_await process->vmes;
      result = try_with_user_page(proof, CR3{process->cr3}, at, count, write,
          [&](char *bytes, uint64_t, uint64_t length) {
            if (write) memcpy(bytes, (char *)kernel + done, length);
            else memcpy((char *)kernel + done, bytes, length);
            return length;
          }, used);
    }
    if (result == PageTry::REFUSED) co_return false;
    if (result == PageTry::NOT_MAPPED) {
      if (!co_await resolve_fault(process, at, write)) co_return false;
    } else done += used;
  }
  co_return true;
}
}

sync<bool> VMM::copy_in(Process process, void *to, VA from, std::size_t n) {
  return copy_user(std::move(process), from, to, n, false);
}

sync<bool> VMM::copy_out(Process process, VA to, const void *from, std::size_t n) {
  return copy_user(std::move(process), to, const_cast<void *>(from), n, true);
}

////////////////////////////// Page faults //////////////////////////////

// A page fault in user mode, handled like a system call.
//   Q: Which stack is it on, and why can't anything live there?
//   Q: Where is the thread's state when it's called?
extern "C" [[noreturn]] void userPageFault(uint64_t cr2, uint64_t error_code) {
  sti();
  impl::start_user_fault(VA{cr2}, error_code);
  CCB::restart_event_loop();
}

// A page fault in kernel mode: always a bug.
//   Q: Why does the kernel never fault on low-mem?
//   Q: Why is high-mem always mapped?
extern "C" [[noreturn]] void kernelPageFault(uint64_t cr2, uint64_t error_code,
                                             uint64_t rip, uint64_t rflags) {
  // An interrupt gate: give interrupts back if the kernel had them on.
  if ((rflags & 0x200) != 0) {
    sti();
  }

  if ((error_code & 1) != 0) {
    KPANIC("protection fault cr2=?, pc=? error_code=?\n", cr2, rip, error_code);
  }

  // the kernel never touches low-mem (see VMM::copy_in)
  const VA va{cr2};
  KPANIC("kernel page fault (?) va=?, pc=? error_code=?\n",
         va >= LOW_MEM_END ? "high-mem" : "low-mem", va.va(), rip, error_code);
}

void VMM::init_system() {
  using namespace impl;

  // interrupts disabled on entry, see pageFaultHandler
  IDT::interrupt(14, uintptr_t(pageFaultHandler_));
}

void do_print(const VPN &vpn) {
  do_print("VPN(");
  do_print(vpn.vpn());
  do_print(")");
}

void do_print(const VA &va) {
  do_print("VA(");
  do_print(va.va());
  do_print(")");
}
