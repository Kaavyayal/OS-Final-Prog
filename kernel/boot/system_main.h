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

#include "arch/x86_64.h"
#include "sched/atomic.h"
#include <cstdint>

namespace impl {
[[noreturn]]
extern void event_loop();
}

namespace Sys {
// The size of every per-core array (core_count is at most this).
constexpr uint64_t MAX_CORES = 64;
// Every core is up, with its CCB: set after the start barrier.
//   Q: What must wait until this is true?
extern bool normal;
extern uint64_t core_count;
extern volatile bool bootstraping;
extern uint64_t start_tsc;
extern uint64_t hhdm_offset;
} // namespace Sys
