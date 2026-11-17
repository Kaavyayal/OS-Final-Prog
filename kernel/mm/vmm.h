#pragma once

#include "fs/ext2.h"
#include "lib/function_ref.h"
#include "lib/shared.h"
#include "mm/physmem.h"
#include "sched/mutex.h"
#include "sched/sync.h"
#include <compare>
#include <cstdint>
#include <expected>
#include <optional>

class VA;

// A virtual page number: bits 12..47 of a virtual address.
//   Q: Why is it masked to 36 bits, and what happens on overflow?
//   Q: How does VA(VPN) turn a high-mem page back into a canonical address?
class VPN {
private:
  static constexpr uint64_t MASK = UINT64_C(0xFFFFFFFFF);
  uint64_t val;

public:
  explicit constexpr inline VPN(const VA &va);
  explicit constexpr inline VPN(uint64_t val) : val(val & MASK) {}
  inline constexpr uint64_t vpn() const { return val; }

  // +
  constexpr VPN operator+(const VPN n) const { return VPN{val + n.val}; }
  template <typename T> constexpr VPN operator+(const T n) const {
    return VPN{val + n};
  }

  // -
  constexpr VPN operator-(const VPN n) const { return VPN{val - n.val}; }
  template <typename T> constexpr VPN operator-(const T n) const {
    return VPN{val - n};
  }

  // ==, !=, <, <=, >, >= ordered by page number
  constexpr auto operator<=>(const VPN &) const = default;

  // same, against a raw page number/count, in either order
  constexpr auto operator<=>(const uint64_t n) const { return val <=> n; }
};

extern void do_print(const VPN &);

class VA {
private:
  uint64_t _va;
  uint64_t check_canonical(uint64_t va);

public:
  constexpr explicit inline VA(const uint64_t va) : _va(va) {}
  // the start of the page, bit 47 sign-extended into a canonical address
  constexpr explicit inline VA(const VPN &vpn)
      : _va(uint64_t(int64_t(vpn.vpn() << (LOG_FRAME_SIZE + 16)) >> 16)) {}
  explicit inline VA(const PA &pa)
      : _va(check_canonical(pa.pa() + Sys::hhdm_offset)) {}
  explicit inline VA(const PPN &ppn) : VA(PA(ppn)) {}
  inline constexpr uint64_t va() const { return _va; }

  template <typename T> operator T *() const { return (T *)_va; }

  inline VPN vpn() const { return VPN(*this); }

  // ==, !=, <, <=, >, >= ordered by address
  constexpr auto operator<=>(const VA &) const = default;
};

extern void do_print(const VA &);

inline constexpr VPN::VPN(const VA &va)
    : val((va.va() >> LOG_FRAME_SIZE) & MASK) {}

// User memory: [VA_START, VA_END).
//   Q: Why is page 0 never mapped?
//   Q: Why does it stop one page short of LOW_MEM_END?
constexpr VA VA_START{UINT64_C(1) << LOG_FRAME_SIZE};
constexpr VA LOW_MEM_END{UINT64_C(1) << 47};
constexpr VA VA_END{LOW_MEM_END.va() - FRAME_SIZE};

namespace impl {
struct VME;
} // namespace impl

// A process: the VMM works on its address space (pcb.h).
class Process;

namespace VMM {

extern void init_system();

// mmap's prot and flags, with Linux's values.
constexpr int PROT_NONE = 0;
constexpr int PROT_READ = 1;
constexpr int PROT_WRITE = 2;
constexpr int PROT_EXEC = 4;

constexpr int MAP_FILE = 0; // Linux compatibility: no effect
constexpr int MAP_SHARED = 0x01;
constexpr int MAP_PRIVATE = 0x02;
constexpr int MAP_TYPE = 0x0f; // shared, private, ...
constexpr int MAP_FIXED = 0x10;
constexpr int MAP_ANONYMOUS = 0x20;

// Linux's mmap, the subset this kernel supports: where it's mapped, or
// Linux's error (lib/errno.h).
//   Q: Which errors can it return, and in which order are they checked?
//   Q: What does MAP_FIXED do to what's already mapped there?
// Shared mappings retain cached frames; private mappings own isolated copies.
extern sync<std::expected<void *, int64_t>>
mmap(Process process, void *addr, std::size_t length, int prot, int flags,
     StrongRef<Node> file, uint64_t offset);

// Linux's munmap.
//   Q: What does it do to a VME that's only partly in the range?
//   Q: When are the unmapped frames freed?
extern sync<int>
munmap(Process process,   // whose address space
       void *addr,        // the address to unmap
       std::size_t length // the length of it that you want to unmap
);

// Copies between the kernel and a process's memory, faulting pages in.
//   Q: Why doesn't the kernel dereference user addresses directly?
//   Q: What has been copied when it returns false?
//   Q: Why must the address space's lock be held during the copy?
extern sync<bool> copy_in(Process process, void *to, VA from, std::size_t n);
extern sync<bool> copy_out(Process process, VA to, const void *from,
                           std::size_t n);

// What with_user_page runs on a page's bytes: f(bytes, done, count).
//   Q: What does f's return value mean?
//   Q: Why must f be quick, and why can't it wait?
using PageBytes =
    FunctionRef<uint64_t(char *bytes, uint64_t done, uint64_t count)>;

// Runs f on the part of [va, va + n) on va's page, if user code could: the
// bytes f used, or nothing if f didn't run (a bad address).
//   Q: Why does it check the access user code would have?
//   Q: What does it wait for?
//   Q: Why can f use fewer bytes than asked?
extern sync<std::optional<uint64_t>>
with_user_page(Process process, VA va, std::size_t n, bool write, PageBytes f);

// Runs with_user_page on each page of [va, va + n): true if it got all of it.
//   Q: Why doesn't it yield between pages, and when would that matter?
extern sync<bool> with_user_pages(Process process, VA va, std::size_t n,
                                  bool write, PageBytes f);

// For Process (process.cc): a process's address space from start to end

// A new address space's VMEs: just the dummy covering page 0.
extern impl::VME *new_vmes();
extern sync<void> fork_memory(Process parent, Process child);
extern sync<void> replace_memory(Process target, Process source);

// A new address space's page tables (its CR3).
//   Q: Why can every address space share the kernel's high-mem tables?
extern uint64_t new_page_tables();

// Frees an address space: its pages, page tables and VMEs.
//   Q: Why doesn't it need a lock?
//   Q: Why doesn't it need a TLB shootdown?
extern void free_memory(uint64_t cr3, Mutex<impl::VME *> &vmes);

// The kernel heap's region, high-mem under one PML4 entry.
//   Q: Why must every address space see the same heap?
constexpr VA HEAP_START{UINT64_C(0xFFFFC00000000000)};
constexpr uint64_t HEAP_BYTES = UINT64_C(4) << 30;

// Creates the heap region's PML4 entry, at boot.
//   Q: Why must it exist before the first process is created?
extern void init_heap_region();

// Maps a page of the heap region to ppn.
//   Q: Who keeps two calls from creating the same page table?
extern void map_heap_page(VPN vpn, PPN ppn);

// Device memory (PCI BARs), right after the heap.
//   Q: Why can't the kernel reach a BAR through the HHDM?
//   Q: Why does it matter that it doesn't share a PDPT entry with the heap?
constexpr VA DEVICE_START{HEAP_START.va() + HEAP_BYTES};
constexpr uint64_t DEVICE_BYTES = UINT64_C(4) << 30;
static_assert(HEAP_BYTES % (UINT64_C(1) << 30) == 0);
static_assert(((DEVICE_START.va() + DEVICE_BYTES - 1) >> 39) ==
              (HEAP_START.va() >> 39));

// Maps device memory at pa and returns where.
//   Q: Does anything ever unmap it?
extern VA map_device(PA pa, uint64_t bytes);

// Applies final ELF segment permissions after loading its bytes.
extern sync<void> protect(Process process, uint64_t start, uint64_t length, int prot);

} // namespace VMM
