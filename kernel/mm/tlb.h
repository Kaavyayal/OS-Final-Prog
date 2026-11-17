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

#pragma once

#include "mm/vmm.h"
#include "sched/atomic.h"
#include "sched/event_loop.h"
#include <coroutine>
#include <cstdint>

// TLB shootdown: makes every core forget stale translations.
//   Q: How does a request reach the other cores, and how do they answer?
//   Q: Why must the handler run even on a core that's busy computing?
//   Q: Why is it safe for the handler to take spin locks?

namespace TLB {

class Shootdown;

// A pending request in a core's mailbox.
struct Node {
  Shootdown *request;
  Node *next;
};

// Each core's pending requests: a lock-free stack.
//   Q: Why can any core push, but only the owner take?
//   Q: Why does taking everything at once avoid ABA?
struct Mailbox {
  Node *head = nullptr;

  void push(Node *node) {
    Node *old = __atomic_load_n(&head, __ATOMIC_RELAXED);
    do {
      node->next = old;
    } while (!__atomic_compare_exchange_n(&head, &old, node, true,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  }

  Node *take_all() {
    if (__atomic_load_n(&head, __ATOMIC_RELAXED) == nullptr) {
      return nullptr;
    }
    return __atomic_exchange_n(&head, nullptr, __ATOMIC_ACQUIRE);
  }
};

// The shootdown IPI's interrupt vector.
constexpr uint8_t VECTOR = 51;

// Installs the IPI handler, once.
//   Q: Why must it come after IDT::init_system?
extern void init_system();

// Registers this core's mailbox.
//   Q: Why must every core do it before running coroutines?
extern void init_core();

// Flushes [start, end) from every core's TLB: co_await it.
//   Q: Why must the PTEs be cleared before, and frames freed only after?
//   Q: What does a core that runs this flush for itself?
class Shootdown {
  const VPN start_;
  const VPN end_;
  // Cores that haven't flushed yet, plus one for the initiator.
  //   Q: What could go wrong without the initiator's extra count?
  Atomic<uint64_t> pending_;
  // The initiator, resumed by whichever core acks last.
  //   Q: In what context does that resume happen?
  Event *waiter_ = nullptr;

  // Posts the request to the other cores: false if they all flushed already.
  bool post(Event *waiter);

public:
  Shootdown(VPN start, VPN end) : start_(start), end_(end), pending_(0) {}
  Shootdown(const Shootdown &) = delete;
  Shootdown &operator=(const Shootdown &) = delete;

  bool await_ready();
  template <Queueable P> bool await_suspend(std::coroutine_handle<P> h) {
    return post(&h.promise());
  }
  void await_resume() {}

  // Flushes this core's TLB and counts it done.
  //   Q: Why can't ack touch *this after it returns?
  void ack();
};

} // namespace TLB
