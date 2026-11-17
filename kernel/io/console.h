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

#include "proc/process.h"
#include "sched/sync.h"
#include <cstddef>

// The user's console, COM2: stdin and stdout (read's fd 0, write's fd 1).
// The kernel's own output (putch, KPRINT, debug.h) and stderr go to COM1.
//   Q: Why give user programs a port of their own?
//   Q: Why can't these busy-wait like putch?
namespace Console {

// Waits for a character and returns it, yielding while there isn't one.
//   Q: Why does it keep the input mutex while it yields?
//   Q: Why not COM1? (what reads it while the machine boots?)
extern sync<char> getch(Process process = {});

// Sends n characters, yielding while the port is busy.
//   Q: Why hold the output mutex for all n, not one at a time?
//   Q: Why a separate mutex from getch's?
extern sync<void> write(const char *chars, std::size_t n);

inline sync<void> putch(const char ch) { co_await write(&ch, 1); }

} // namespace Console
