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

#include "fs/ext2.h"
#include "lib/shared.h"
#include "mm/vmm.h"
#include "sched/sync.h"
#include <cstdint>

// A minimal ELF loader: static x86-64 executables, PT_LOAD only.
//   Q: What does it skip that a dynamically linked program would need?
namespace ELF {

// Maps file's PT_LOAD segments into process; returns the entry or 0.
//   Q: How does it use mmap to map each segment?
//   Q: Where does .bss come from?
extern sync<uint64_t> load(Process process, StrongRef<Node> file);

} // namespace ELF
