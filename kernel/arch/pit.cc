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

#include "arch/pit.h"

#include "arch/idt.h"
#include "arch/machine.h"
#include "arch/x86_64.h"
#include "lib/print.h"
#include "proc/ccb.h"
#include "proc/tcb.h"

/*
 * The old PIT runs at a known frequency (1193182Hz) but there's one for the
 * whole machine. Every core has its own APIT, but we don't know how fast it
 * counts.
 *
 * The plan: time the APIT against the PIT once, then program every core's
 * APIT for the frequency we want and abandon the PIT.
 *
 * An emulator never gets timing exactly right: the result is approximate.
 */

namespace {

// The PIT's fixed input frequency.
constexpr uint32_t PIT_FREQ = 1193182;

// The PIT's rate while calibrating: its divider must fit in 16 bits.
//   Q: What's the slowest rate the PIT can do?
constexpr uint32_t CALIBRATION_HZ = 20;
static_assert(PIT_FREQ / CALIBRATION_HZ <= 0xFFFF);

// How many of its periods we time the APIT for (1/20 second each).
//   Q: What does timing for longer buy, and what does it cost?
constexpr uint32_t CALIBRATION_PERIODS = 2;

// Where the APIT interrupts us.
constexpr uint8_t VECTOR = 40;

// The APIT's initial count for one tick: set by init_system.
uint32_t apit_count = 0;

} // namespace

Atomic<uint64_t> Pit::jiffies{0};
uint32_t Pit::jiffies_per_second = 0;

void Pit::init_system(uint32_t hz) {
  ASSERT(hz > 0);

  // Lets the APIT count down from the top, masked, divide by 1.
  Msr::LVTTimerRegister().write(0x00010000);
  Msr::DivideConfigurationRegister().write(0x0000000B);

  constexpr uint16_t divider = PIT_FREQ / CALIBRATION_HZ;
  KPRINT("| Pit: calibrating, PIT divider ?\n", Dec(divider));

  const uint32_t initial = 0xFFFFFFFF;
  Msr::InitialCountRegister().write(initial);

  outb(0x61, 1); // speaker off, channel 2's gate on

  outb(0x43, 0b10110110); //  10 -> channel 2
                          //  11 -> lobyte/hibyte
                          // 011 -> square wave generator
                          //   0 -> count in binary
  outb(0x42, uint8_t(divider));
  outb(0x42, uint8_t(divider >> 8));

  // In square-wave mode, channel 2's output (bit 5 of port 0x61) changes
  // twice per period.
  //   Q: Why poll the output instead of taking the PIT's interrupt?
  uint32_t last = inb(0x61) & 0x20;
  uint32_t changes = 0;
  while (changes < 2 * CALIBRATION_PERIODS) {
    const uint32_t now = inb(0x61) & 0x20;
    if (now != last) {
      changes++;
      last = now;
    }
  }

  const uint64_t counted = initial - Msr::CurrentCountRegister().read();

  outb(0x61, 0); // stop the PIT

  const uint64_t apit_hz = counted * CALIBRATION_HZ / CALIBRATION_PERIODS;
  ASSERT(apit_hz / hz > 0 && apit_hz / hz <= 0xFFFFFFFF);
  apit_count = uint32_t(apit_hz / hz);
  jiffies_per_second = hz;
  KPRINT("| Pit: APIT runs at ?Hz, count ? for ?Hz\n", Dec(apit_hz),
         Dec(apit_count), Dec(hz));

  IDT::interrupt(VECTOR, uintptr_t(apitHandler_));
}

void Pit::init_core() {
  ASSERT(apit_count != 0); // init_system first

  Msr::DivideConfigurationRegister().write(0x0000000B); // divide by 1

  // Interrupts start as soon as the core's IF is on.
  Msr::LVTTimerRegister().write((1 << 17) | // periodic
                                (0 << 16) | // not masked
                                VECTOR);
  Msr::InitialCountRegister().write(apit_count);
}

// The APIT's handler, called from apitHandler_ (machine.S) with interrupts
// off.
//   Q: Why doesn't it switch threads (what would preemption need here)?
extern "C" void apitHandler() {
  if (CCB::current()->id == 0) {
    Pit::jiffies.add_fetch(1);
  }
  Msr::EOI().write(0);
}

namespace {
[[gnu::noinline]] void reschedule_user() {
  Thread::current().schedule(true);
}
}
extern "C" [[noreturn]] void userTimerInterrupt() {
  apitHandler();
  sti();
  reschedule_user();
  CCB::restart_event_loop();
}
