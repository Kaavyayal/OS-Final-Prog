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

#include "sched/barrier.h"

bool Barrier::Arrive::await_ready() noexcept {
  LockGuard g{barrier.spin};
  ASSERT(barrier.togo > 0);
  barrier.togo -= 1;
  if (barrier.togo != 0) {
    return false;
  }
  // the last one: everybody goes on
  Event *event = barrier.waiters.remove_all();
  while (event != nullptr) {
    // next first: once it's queued, it can run and be gone
    Event *const next = event->next;
    impl::ready_queue.add(event);
    event = next;
  }
  return true;
}

bool Barrier::Arrive::wait(Event *waiter) noexcept {
  LockGuard g{barrier.spin};
  if (barrier.togo == 0) {
    return false;
  }
  barrier.waiters.add(waiter);
  return true;
}
