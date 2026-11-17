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

#include "sched/atomic.h"
#include <cstdint>

// Time: every core's local APIC timer (the APIT) interrupts it periodically,
// calibrated against the old PIT.
//   Q: Why not use the PIT's interrupts directly?
//   Q: Why calibrate at all, what don't we know about the APIT?
namespace Pit {

// Ticks since the timer started, counted by core 0.
//   Q: Why does only one core count them?
extern Atomic<uint64_t> jiffies;

// Ticks per second: the hz init_system was given.
extern uint32_t jiffies_per_second;

// Calibrates the APIT for hz interrupts per second and installs its handler,
// on the BSP while it boots.
//   Q: Why must it come after IDT::init_system?
extern void init_system(uint32_t hz);

// Starts this core's APIT.
//   Q: Why must it come after IDT::init_core?
extern void init_core();

} // namespace Pit
