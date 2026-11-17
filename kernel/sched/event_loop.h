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

#include "lib/shared.h"
#include "sched/atomic.h"
#include "sched/event.h"
#include "sched/queue.h"
#include "sched/spin_lock.h"
#include <coroutine>

namespace impl {
extern Queue<Event, SpinLock> preempted_queue;

// The event loop's work: coroutines and threads to resume, in order.
//   Q: What happens to queued work while a core runs user code?
//   Q: How could every core get stuck at once?
extern Queue<Event, SpinLock> ready_queue;

[[noreturn]] void event_loop(bool shutdown_when_done);
extern Atomic<uint64_t> started;
extern Atomic<uint64_t> finished;

} // namespace impl
