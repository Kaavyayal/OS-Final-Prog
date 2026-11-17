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

#include "lib/debug.h"

#include "arch/machine.h"
#include "boot/system_main.h"
#include "lib/print.h"
#include "sched/spin_lock.h"
#include <cstdint>

namespace impl {
SpinLock putch_spin{};
}

void putch(const char ch) {
  using namespace impl;

  while (true) {
    putch_spin.lock();
    if (inb(0x3F8 + 5) & 0x20)
      break;
    putch_spin.unlock();
  }

  outb(0x3F8, (std::uint8_t)ch);
  putch_spin.unlock();
}

void puts(const char *str) {
  while (*str) {
    putch(*str);
    str++;
  }
}

[[noreturn]]
void shutdown(bool show_stats) {
  auto total_tsc = Sys::core_count * (rdtsc() - Sys::start_tsc);
  if (show_stats) {
    // SAY("checking leaks\n");
    // impl::check_leaks();
    SpinLock::print_stats();
    KPRINT("@@@ total_tsc         = ?\n", Dec(total_tsc));
    if (total_tsc != 0) {
      KPRINT("@@@ critical_percentage = ?\n",
             Dec((100 * SpinLock::total_cycles()) / total_tsc));
    }
  }
  while (true) {
    outb(0xf4, 0x00);
    asm("hlt");
  }
}

[[noreturn]]
void assert(const char *file, int line, const char *cond) {
  KPRINT("assertion failed: ? ? ?\n", file, Dec(line), cond);
  shutdown(false);
}

extern "C" {

[[noreturn]]
void abort() {
  KPANIC("?\n", "abort");
}
}

namespace std {

void __glibcxx_assert_fail(const char *file, int line, const char *function,
                           const char *condition) noexcept {
  KPANIC("libstdc++ assertion failed: ? ? ? ?\n", file, Dec(line), function,
         condition);
  shutdown(false);
  while (true)
    pause();
}
} // namespace std

// The C++ runtime's hook for statics with destructors: does nothing.
//   Q: Why is registering destructors pointless in a kernel?
extern "C" int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
extern "C" {
void *__dso_handle = nullptr;
}

extern "C" [[noreturn]] void __cxa_pure_virtual() {
  KPANIC("?\n", "pure virtual function called");
}
