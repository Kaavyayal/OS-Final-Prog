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

#include "boot/core_main.h"
#include "arch/idt.h"
#include "arch/machine.h"
#include "arch/pit.h"
#include "arch/x86_64.h"
#include "boot/kernel_main.h"
#include "boot/limine.h"
#include "boot/system_main.h"
#include "lib/print.h"
#include "mm/tlb.h"
#include "proc/ccb.h"
#include "syscall/syscall.h"

SpinBarrier *impl::start_barrier;

[[noreturn]] void impl::ap_main(limine_mp_info *info) {
  // Checks this core's CPU features and enables the ones needed.
  Features features{};
  ASSERT(features.hasSSE3);
  enable_sse();
  ASSERT(features.hasFSGSBASE);
  CR4().setFSGSBASE();
  // The kernel never touches user memory directly: SMAP enforces it.
  //   Q: How do copy_in and copy_out get around SMAP?
  //   Q: Why do the ways in from user mode clear RFLAGS.AC?
  ASSERT(features.hasSMAP);
  CR4().setSMAP();

  // This core's CCB, in ap_main's frame, which never goes away.
  //   Q: Why is a frame of a function that never returns a safe home?
  CCB::Storage ccb;
  CCB::init_core(ccb, uint32_t(info->extra_argument), info->lapic_id);

  core_main(info);
}

[[noreturn]] void impl::core_main(limine_mp_info *info) {

  // Limine's stack below core_main's frame: free to throw away.
  //   Q: Why can everything above this point be discarded?
  uint64_t stack_top;
  asm volatile("mov %%rsp, %0" : "=r"(stack_top));
  stack_top &= ~UINT64_C(15);

  CCB::start_core(stack_top);

  auto old_efer = Msr::EFER().read();
  KPRINT("old efer ?\n", old_efer);
  Msr::EFER().write(old_efer | 1);
  auto new_efer = Msr::EFER().read();
  KPRINT("new efer ?\n", new_efer);

  Syscall::init_core();

  IDT::init_core();
  TLB::init_core();
  Pit::init_core();

  // APIC and IDT are ready: pending shootdown IPIs and timer ticks arrive now.
  sti();

  KPRINT("bootstrapped core ? (lapic_id ?)\n", Dec(CCB::current()->id),
         Dec(info->lapic_id));

  impl::start_barrier->sync();
  // every core has its CCB now
  Sys::normal = true;

  if (info->lapic_id == 0) {
    /* run the async kernel_main */
    kernel_main();
  }

  impl::event_loop(info->lapic_id == 0);
}
