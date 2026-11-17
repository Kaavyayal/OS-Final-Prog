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

#include "boot/system_main.h"

#include "arch/idt.h"
#include "arch/pit.h"
#include "arch/x86_64.h"
#include "boot/core_main.h"
#include "boot/limine++.h"
#include "lib/debug.h"
#include "lib/print.h"
#include "mm/heap.h"
#include "mm/physmem.h"
#include "mm/tlb.h"
#include "mm/vmm.h"
#include "proc/ccb.h"
#include "proc/thread.h"
#include <cstdint>

using namespace limine;

extern void (*_init_array_start)();
extern void (*_init_array_end)();

[[gnu::section(
    ".limine_requests")]] constexpr volatile BaseRevision base_revision{};
[[gnu::section(".limine_requests")]] constexpr volatile MultiProcessor mp{};
[[gnu::section(".limine_requests")]] constexpr volatile HHDM limine_hhdm{};

uint64_t Sys::core_count = 0;
volatile bool Sys::bootstraping = true;
uint64_t Sys::start_tsc = 0;
bool Sys::normal = false;
uint64_t Sys::hhdm_offset = 0;

extern "C" [[noreturn]] void system_main(void) {

  using namespace Sys;

  // Checks that Limine supports the base revision we need.
  ASSERT(base_revision.supported());

  ///////////////////////////////////////////////////////////
  // Check supported CPU features and enable required ones //
  ///////////////////////////////////////////////////////////

  Features features{};
  features.dump();
  ASSERT(features.hasSSE3);
  enable_sse();
  ASSERT(features.hasFSGSBASE);
  CR4().setFSGSBASE();
  // The kernel never touches user memory directly: SMAP enforces it.
  //   Q: How do copy_in and copy_out get around SMAP?
  //   Q: Why do the ways in from user mode clear RFLAGS.AC?
  ASSERT(features.hasSMAP);
  CR4().setSMAP();
  CR0().dump();
  CR4().dump();

  start_tsc = rdtsc();

  Sys::hhdm_offset = limine_hhdm.response->offset;

  ///////////////////////////
  // Initialize Global IDT //
  ///////////////////////////

  IDT::init_system();

  ////////////////////////////////////////
  // Initialize the TLB shootdown logic //
  ////////////////////////////////////////

  TLB::init_system();

  //////////////////////////////////////////
  // Calibrate the APIC timer (the APIT)  //
  //////////////////////////////////////////

  Pit::init_system(1000);

  ////////////////////
  // Initialize VMM //
  ////////////////////

  VMM::init_system();

  ///////////////////////////
  // Initialize core count //
  ///////////////////////////

  ASSERT(mp.response);

  core_count = mp.response->cpu_count;
  // x2APIC: local APIC registers through MSRs, not MMIO.
  //   Q: Why is that simpler than memory-mapped APIC registers?
  ASSERT(mp.response->flags & LIMINE_MP_RESPONSE_X86_64_X2APIC);

  /////////////////////////////////////////////////////////////
  // Physical memory and the heap: before anything allocates //
  /////////////////////////////////////////////////////////////

  // The BSP's CCB, in system_main's frame, which never goes away.
  uint64_t bsp_index = UINT64_MAX;
  for (uint64_t i = 0; i < core_count; i++) {
    if (mp.response->cpus[i]->lapic_id == mp.response->bsp_lapic_id) {
      bsp_index = i;
    }
  }
  ASSERT(bsp_index != UINT64_MAX);
  CCB::Storage bsp_ccb;
  CCB::init_core(bsp_ccb, uint32_t(bsp_index), mp.response->bsp_lapic_id);

  physMem.init();
  heap::init_system();

  /////////////////////////////
  // Run global initializers //
  /////////////////////////////

  KPRINT("_init_array_start:? _init_array_end:?\n", &_init_array_start,
         &_init_array_end);

  std::size_t count = &_init_array_end - &_init_array_start;
  for (std::size_t i = 0; i < count; i++) {
    (*((&_init_array_start)[i]))();
  }

  // The kernel thread: every core runs as it until it runs a user one.
  Thread::init_system();

  /////////////////////////
  // Wake up other cores //
  /////////////////////////

  KPRINT("has ? cores, flags=?, bsp_lapic_id=?\n", Dec(core_count),
         mp.response->flags, mp.response->bsp_lapic_id);

  impl::start_barrier = new SpinBarrier(core_count);

  for (uint64_t i = 0; i < core_count; i++) {
    auto info = mp.response->cpus[i];
    if (i != bsp_index) {
      // becomes the core's CCB::id
      info->extra_argument = i;
      info->goto_address = impl::ap_main;
    }
  }

  KPRINT("bootstrapping core index=? lapic_id=?\n", Dec(bsp_index),
         Dec(mp.response->cpus[bsp_index]->lapic_id));

  impl::core_main(mp.response->cpus[bsp_index]);
}
