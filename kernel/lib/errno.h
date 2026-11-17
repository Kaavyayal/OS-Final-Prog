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

#include <cstdint>

// Linux's error numbers, negated: what a failing system call returns (libc
// turns -e into -1 and errno = e).
namespace Errno {
constexpr int64_t EPERM = -1;
constexpr int64_t E2BIG = -7;
constexpr int64_t EBADF = -9;
constexpr int64_t EAGAIN = -11;
constexpr int64_t ENOMEM = -12;
constexpr int64_t EFAULT = -14;
constexpr int64_t EINVAL = -22;
constexpr int64_t ENOSYS = -38;
constexpr int64_t EOVERFLOW = -75;
} // namespace Errno
