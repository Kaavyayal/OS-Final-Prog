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

#include "io/console.h"

#include "arch/machine.h"
#include "proc/pcb.h"
#include "sched/async.h"
#include "sched/mutex.h"
#include <cstdint>

namespace {

// COM2's registers.
//   Q: Which bits of the line status register say a character is waiting,
//      and that the port can take another?
constexpr uint16_t COM2 = 0x2F8;
constexpr uint16_t LSR = COM2 + 5;

// COM2's two sides, each reachable only through its mutex's Proof.
class Input {
public:
  bool eof = false;
  bool ready() const { return inb(LSR) & 0x01; }
  char read() const { return char(inb(COM2)); }
};

class Output {
public:
  bool ready() const { return inb(LSR) & 0x20; }
  void write(const char ch) const { outb(COM2, uint8_t(ch)); }
};

Mutex<Input> input{};
Mutex<Output> output{};

} // namespace

sync<char> Console::getch(Process process) {
  auto proof = co_await input; // not const: Proof::data() isn't
  if (proof.data().eof)
    co_return char(4);
  while (!proof.data().ready()) {
    if (process != nullptr && process->exiting())
      co_return char(4);
    co_await yield();
  }
  char ch = proof.data().read();
  if (ch == 4)
    proof.data().eof = true;
  co_return ch;
}

sync<void> Console::write(const char *chars, std::size_t n) {
  auto proof = co_await output; // not const: Proof::data() isn't
  for (std::size_t i = 0; i < n; i++) {
    while (!proof.data().ready()) {
      co_await yield();
    }
    proof.data().write(chars[i]);
  }
}
