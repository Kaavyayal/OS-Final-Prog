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

#include "sched/spin_lock.h"

#include "boot/system_main.h"
#include "lib/print.h"
#include "proc/ccb.h"
#include "sched/atomic.h"

namespace {

// One core's critical-section statistics.
//   Q: Why are plain adds safe here, without atomics?
//   Q: Why does each core get its own cache line?
struct alignas(64) Stats {
  uint64_t cycles = 0;
  uint64_t count = 0;
  uint64_t max = 0;
};
Stats stats[Sys::MAX_CORES];

// Every core's statistics, added up (racy while cores run).
Stats totals() {
  Stats all{};
  for (uint64_t i = 0; i < Sys::core_count; i++) {
    all.cycles += stats[i].cycles;
    all.count += stats[i].count;
    all.max = stats[i].max > all.max ? stats[i].max : all.max;
  }
  return all;
}

} // namespace

uint64_t SpinLock::total_cycles() { return totals().cycles; }

void SpinLock::print_stats() {
  const Stats all = totals();
  KPRINT("@@@ max_spin_cycles        = ?\n", Dec(all.max));
  KPRINT("@@@ critical_section_count = ?\n", Dec(all.count));
  if (all.count != 0) {
    KPRINT("@@@ avg_spin_cycles        = ?\n", Dec(all.cycles / all.count));
  }
  KPRINT("@@@ total_critial_section_cycles = ?\n", Dec(all.cycles));
}

bool SpinLock::tryLock() {
  auto was = disable();
  auto has_it = !taken.exchange(true);
  if (has_it) {
    was_disabled = was;
    start = rdtsc();
  } else {
    restore(was);
  }
  return has_it;
}

void SpinLock::lock() {
  while (!tryLock()) {
    stuckInALoop();
  }
}

void SpinLock::unlock() {
  const uint64_t elapsed = rdtsc() - start;
  const bool was = was_disabled;
  taken.set(false);
  // Still disabled; only after boot (before, a core may lack a CCB).
  if (Sys::normal) {
    Stats &mine = stats[CCB::current()->id];
    mine.cycles += elapsed;
    mine.count += 1;
    if (elapsed > mine.max) {
      mine.max = elapsed;
    }
  }
  restore(was);
}
