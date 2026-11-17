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

#include "proc/ccb.h"

#include "arch/machine.h"
#include "proc/pcb.h"
#include "proc/tcb.h"
#include "sched/event_loop.h"
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

// The entry code (machine.S, CCB_*) has these offsets built in.
//   Q: What breaks in machine.S if a field moves?
static_assert(std::is_standard_layout_v<CCB>);
static_assert(offsetof(CCB, stack_top) == 0x10);
static_assert(offsetof(CCB, user_rsp) == 0x18);
static_assert(offsetof(CCB, running) == 0x20);

CCB::CCB(uint32_t id, uint32_t apic_id)
    : self(this), id(id), apic_id(apic_id) {}

void CCB::Running::set(Thread thread) {
  saved_state_ = &thread->state;
  // The new thread's CR3, loaded before letting go of the old thread.
  const uint64_t cr3 = thread->process->cr3;
  if (get_cr3() != cr3) {
    set_cr3(cr3);
  }
  // last: letting go of the old thread can delete it
  thread_ = std::move(thread);
}

void CCB::init_core(Storage &storage, uint32_t id, uint32_t apic_id) {
  wrgsbase(uintptr_t(new (storage.bytes) CCB(id, apic_id)));
}

void CCB::start_core(uint64_t stack_top) {
  CCB *ccb = current();
  // the event loop's stack would run over it
  ASSERT(uintptr_t(ccb) >= stack_top);
  ccb->stack_top = stack_top;
  // interrupts and exceptions in user mode start on a fresh stack
  ccb->gdt.tss.rsp[0] = stack_top;
  ccb->gdt.load();
  // until it runs a user thread
  Thread::kernel().enter();
}

void CCB::restart_event_loop() {
  CCB *ccb = CCB::current();
  // Let go of an ended thread; a live one stays to save CR3 switches.
  if (ccb->running.thread()->ended()) {
    Thread::kernel().enter();
  }
  call_on_stack_(
      nullptr,
      [](void *) {
        // the same core runs the shutdown as in core_main
        impl::event_loop(CCB::current()->apic_id == 0);
      },
      (void *)ccb->stack_top);
  __builtin_unreachable();
}
