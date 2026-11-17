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

#include <cstdint>

#include "boot/limine.h"
#include "sched/spin_barrier.h"

namespace impl {
extern SpinBarrier *start_barrier;
// Where Limine starts the other cores (APs).
//   Q: What does it set up before core_main?
[[noreturn]] extern void ap_main(limine_mp_info *info);
// Every core ends up here, with its CCB.
//   Q: What does the BSP do here that the others don't?
[[noreturn]] extern void core_main(limine_mp_info *info);
} // namespace impl
