/* Copyright (C) 2026 Ahmed Gheith and contributors.
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

#include "arch/gdt.h"
#include "arch/saved_state.h"
#include "lib/shared.h"
#include "mm/heap.h"
#include "mm/tlb.h"
#include "proc/thread.h"
#include <cstddef>
#include <cstdint>
#include <utility>

// Core control block: per-core data, reached through GS.
//   Q: Why GS rather than a global array indexed by core id?
//   Q: Which fields does assembly code read, and how does it find them?
struct CCB {
  CCB *self;        // has to be first, current() reads GS:0
  uint32_t id;      // 0 .. Sys::core_count - 1
  uint32_t apic_id; // the local APIC's ID

  // Must stay standard-layout: machine.S uses fixed offsets (see ccb.cc).

  // The top of this core's stack.
  //   Q: Why does every entry into the kernel start over at the top?
  //   Q: What happens to the stack when the core enters user mode?
  uint64_t stack_top = 0;
  // Scratch for the system call entry: the user's RSP.
  uint64_t user_rsp = 0;

  // The thread this core runs as, and where its user state is saved.
  //   Q: Why is there only one setter?
  //   Q: Why can't the SavedState * belong to a different thread?
  class Running {
    // First field (offset 0): the entry code reads it (CCB_SAVED_STATE).
    SavedState *saved_state_ = nullptr;
    // Keeps the thread, its state and its process's memory alive.
    //   Q: What would go wrong if this were a plain pointer?
    Thread thread_{};

  public:
    const Thread &thread() const { return thread_; }
    SavedState *saved_state() const { return saved_state_; }

    // Sets the thread, loading its process's page tables.
    //   Q: Why is this the only place CR3 is written?
    //   Q: Why does a CR3 write flush the old process's translations?
    void set(Thread thread);
  };
  Running running{};

  TLB::Mailbox tlb_mailbox; // shootdown requests for this core

  // The slab this core allocates from (heap.cc).
  //   Q: Why must interrupts be off while it changes?
  heap::Slab *slab = nullptr;
  uint32_t preferred_events = 0;

  GDT gdt; // this core's GDT and TSS

  CCB(uint32_t id, uint32_t apic_id);
  CCB() = delete;
  CCB(const CCB &) = delete;
  CCB &operator=(const CCB &) = delete;

  // The current core's CCB.
  //   Q: Why must you not keep it across a co_await?
  static CCB *current() {
    CCB *ccb;
    asm volatile("mov %%gs:0, %0" : "=r"(ccb));
    return ccb;
  }

  // Room for a CCB, which is never destroyed.
  struct Storage;

  // Builds this core's CCB in storage and points GS at it.
  //   Q: Why must it run before anything allocates?
  //   Q: Where does storage live, and why must it never move?
  static void init_core(Storage &storage, uint32_t id, uint32_t apic_id);

  // Finishes core setup: stack, GDT, TSS, and the kernel thread.
  //   Q: Why must the kernel thread exist first?
  static void start_core(uint64_t stack_top);

  // Throws away this core's stack and restarts the event loop.
  //   Q: Why must nothing live be on the stack when it's called?
  [[noreturn]] static void restart_event_loop();
};

struct CCB::Storage {
  alignas(CCB) std::byte bytes[sizeof(CCB)];
};
