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

#include "sched/event_loop.h"
#include "lib/print.h"
#include "proc/ccb.h"
#include "proc/tcb.h"

namespace impl {

Queue<Event, SpinLock> ready_queue{};
Queue<Event, SpinLock> preempted_queue{};
Atomic<uint64_t> started = 0;
Atomic<uint64_t> finished = 0;

[[noreturn]]
void event_loop(bool shutdown_when_done) {
  while ((started == 0) || (finished != started)) {
    // Queue locks mask interrupts. Allow a full instruction window outside
    // their STI shadow so timer delivery progresses under kernel-only load.
    asm volatile("sti; nop; nop" ::: "memory");
    // Drop a finished thread even when other runnable work keeps the
    // queue nonempty: its last reference must not delay child reaping.
    if (CCB::current()->running.thread()->ended())
      Thread::kernel().enter();
    // Favor work that blocks or yields over threads consuming full quanta.
    // Periodically serve the latter even when the preferred queue stays busy.
    auto &count = CCB::current()->preferred_events;
    Event *event = nullptr;
    if (count >= 64) {
      event = preempted_queue.remove();
      count = 0;
    }
    if (!event) {
      event = ready_queue.remove();
      if (event)
        ++count;
    }
    if (!event) {
      event = preempted_queue.remove();
      count = 0;
    }
    if (event) {
      event->dispatch();
    } else {
      // Leave an interruptible idle window even with no runnable user.
      // The periodic APIC interrupt also bounds remote-queue wake latency.
      asm volatile("sti; hlt" ::: "memory");
      // Idle: let go of an ended thread so its memory can be freed.
      if (CCB::current()->running.thread()->ended()) {
        Thread::kernel().enter();
      }
    }
  }
  if (shutdown_when_done) {
    KPRINT("event_loop: all done started=? finished=?, shutting down\n",
           Dec(started.get()), Dec(finished.get()));
    shutdown(true);
  }
  while (true) {
    asm volatile("hlt");
  }
}

} // namespace impl
