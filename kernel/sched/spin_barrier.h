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
#include "sched/spin_lock.h"
#include <cstdint>

class SpinBarrier {
  SpinLock spin;
  std::uint64_t togo;

public:
  SpinBarrier(std::uint64_t count) : togo(count) {}
  [[gnu::noinline]]
  void sync() {

    spin.lock();
    if (togo == 0) {
      KPANIC("?\n",
             "Barrier::sync() called after barrier was already released");
    }
    togo -= 1;
    spin.unlock();
    while (togo != 0) {
      stuckInALoop();
    }
  }
};
