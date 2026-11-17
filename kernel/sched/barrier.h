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

#include "lib/print.h"
#include "sched/atomic.h"
#include "sched/event_loop.h"
#include "sched/spin_lock.h"
#include <cstdint>

// A barrier that blocks coroutines until count of them arrive.
//   Q: Who wakes the waiters up?
//   Q: Can it be used a second time?
class Barrier {
  SpinLock spin{};
  std::uint64_t togo;
  Queue<Event, NoLock> waiters{}; // the coroutines waiting at it

public:
  // Waits for count arrivals.
  explicit Barrier(std::uint64_t count) : togo(count) {}

  // co_await barrier: arrive, and wait for everyone else.
  //   Q: What does the last one to arrive do differently?
  class Arrive {
    Barrier &barrier;

    // Waits at the barrier, unless it opened meanwhile (false).
    bool wait(Event *waiter) noexcept;

  public:
    explicit Arrive(Barrier &barrier) : barrier(barrier) {}
    bool await_ready() noexcept;
    template <Queueable P>
    bool await_suspend(std::coroutine_handle<P> h) noexcept {
      return wait(&h.promise());
    }
    void await_resume() noexcept {}
  };

  Arrive operator co_await() noexcept { return Arrive{*this}; }
};
