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

#include "mm/tlb.h"

#include "arch/idt.h"
#include "arch/machine.h"
#include "arch/x86_64.h"
#include "boot/system_main.h"
#include "proc/ccb.h"
#include "sched/event_loop.h"

namespace TLB {

namespace {

// Above this many pages, reloading CR3 is cheaper than invlpg.
//   Q: Why does reloading CR3 not flush the kernel's high-mem pages?
constexpr uint64_t INVLPG_LIMIT = 32;

// Every core's mailbox, so other cores can post to it.
Mailbox *mailboxes[Sys::MAX_CORES];

void flush(VPN start, VPN end) {
  if ((end - start).vpn() > INVLPG_LIMIT) {
    // low-mem mappings aren't global
    set_cr3(get_cr3());
    return;
  }
  for (VPN vpn = start; vpn < end; vpn = vpn + 1) {
    // VA{vpn} sign-extends, high-mem pages work too
    asm volatile("invlpg (%0)" : : "r"(VA{vpn}.va()) : "memory");
  }
}

// Services this core's pending requests.
//   Q: Who calls it, and with interrupts on or off?
void poll() {
  Node *node = CCB::current()->tlb_mailbox.take_all();
  while (node != nullptr) {
    Node *next = node->next;
    node->request->ack();
    delete node;
    node = next;
  }
}

} // namespace

void init_system() { IDT::interrupt(VECTOR, uintptr_t(tlbHandler_)); }

// Registers this core's mailbox.
//   Q: Why can nothing post to it before the start barrier?
void init_core() {
  ASSERT(Sys::core_count <= Sys::MAX_CORES);
  const uint32_t id = CCB::current()->id;
  ASSERT(id < Sys::core_count);
  mailboxes[id] = &CCB::current()->tlb_mailbox;
}

bool Shootdown::await_ready() {
  flush(start_, end_);
  return Sys::core_count == 1;
}

bool Shootdown::post(Event *waiter) {
  waiter_ = waiter;
  // The extra count keeps a fast core from resuming us mid-post.
  pending_.set(Sys::core_count);
  Mailbox *const mine = &CCB::current()->tlb_mailbox;
  for (uint64_t i = 0; i < Sys::core_count; i++) {
    if (mailboxes[i] != mine) {
      mailboxes[i]->push(new Node{this, nullptr});
    }
  }
  // Makes the requests visible before the IPI arrives.
  asm volatile("mfence; lfence" ::: "memory");
  // fixed delivery, assert, all excluding self
  Msr::InterruptCommandRegister().write((3 << 18) | (1 << 14) | VECTOR);
  // false: everyone already flushed, no need to suspend.
  return pending_.sub_fetch(1) != 0;
}

void Shootdown::ack() {
  flush(start_, end_);
  if (pending_.sub_fetch(1) == 0) {
    impl::ready_queue.add(waiter_);
  }
}

} // namespace TLB

// The shootdown IPI's handler, called from tlbHandler_ (machine.S).
//   Q: What does it do besides poll?
extern "C" void tlbHandler() {
  TLB::poll();
  Msr::EOI().write(0);
}
