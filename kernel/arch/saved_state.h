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

// A user thread's saved state, shared with machine.S.
//   Q: Which ways into the kernel save it?
//   Q: How does machine.S know the offsets, and what checks them?

#define SS_RAX 0x00
#define SS_RBX 0x08
#define SS_RCX 0x10
#define SS_RDX 0x18
#define SS_RSI 0x20
#define SS_RDI 0x28
#define SS_RBP 0x30
#define SS_R8 0x38
#define SS_R9 0x40
#define SS_R10 0x48
#define SS_R11 0x50
#define SS_R12 0x58
#define SS_R13 0x60
#define SS_R14 0x68
#define SS_R15 0x70
#define SS_RIP 0x78
#define SS_RSP 0x80
#define SS_RFLAGS 0x88
#define SS_FS_BASE 0x90
#define SS_GS_BASE 0x98
#define SS_FPU 0xA0

#ifndef __ASSEMBLER__

#include <cstddef>
#include <cstdint>
#include <type_traits>

// fxrstor's image of a fresh x87/SSE state (machine.S).
extern "C" const uint8_t clean_fpu[512];

struct alignas(16) SavedState {
  // The GPRs
  uint64_t rax = 0;
  uint64_t rbx = 0;
  uint64_t rcx = 0;
  uint64_t rdx = 0;
  uint64_t rsi = 0;
  uint64_t rdi = 0;
  uint64_t rbp = 0;
  uint64_t r8 = 0;
  uint64_t r9 = 0;
  uint64_t r10 = 0;
  uint64_t r11 = 0;
  uint64_t r12 = 0;
  uint64_t r13 = 0;
  uint64_t r14 = 0;
  uint64_t r15 = 0;
  // Where it goes on: every entry saves these.
  uint64_t rip = 0;
  uint64_t rsp = 0;
  uint64_t rflags = 0x202; // IF
  uint64_t fs_base = 0;
  uint64_t gs_base = 0;
  // x87/SSE state in fxsave format (16-byte aligned).
  alignas(16) uint8_t fpu[512];

  // System call argument N (0-5) as a T.
  //   Q: Which registers hold the arguments, and why R10 instead of RCX?
  //   Q: Why is a user address a VA and never a pointer?
  template <unsigned N, typename T = uint64_t> T arg() const {
    static_assert(N < 6, "system calls have 6 arguments");
    static_assert(!std::is_pointer_v<T>,
                  "a user address is a VA (or an integer), not a pointer");
    const uint64_t args[] = {rdi, rsi, rdx, r10, r8, r9};
    return static_cast<T>(args[N]);
  }

  // A new thread's state: the reset defaults.
  //   Q: What must be set before it runs?
  SavedState() {
    for (std::size_t i = 0; i < sizeof(fpu); i++) {
      fpu[i] = clean_fpu[i];
    }
  }

  // Not copyable: it's big (the FPU state alone is 512 bytes), and it's always
  // reached through a pointer.
  //   Q: What could an accidental copy break, besides the time it takes?
  SavedState(const SavedState &) = delete;
  SavedState &operator=(const SavedState &) = delete;

  // The one deliberate copy: clone's child starts where its parent is.
  void copy_from(const SavedState &other) {
    __builtin_memcpy(static_cast<void *>(this),
                     static_cast<const void *>(&other), sizeof(SavedState));
  }
};

static_assert(offsetof(SavedState, rax) == SS_RAX);
static_assert(offsetof(SavedState, rbx) == SS_RBX);
static_assert(offsetof(SavedState, rcx) == SS_RCX);
static_assert(offsetof(SavedState, rdx) == SS_RDX);
static_assert(offsetof(SavedState, rsi) == SS_RSI);
static_assert(offsetof(SavedState, rdi) == SS_RDI);
static_assert(offsetof(SavedState, rbp) == SS_RBP);
static_assert(offsetof(SavedState, r8) == SS_R8);
static_assert(offsetof(SavedState, r9) == SS_R9);
static_assert(offsetof(SavedState, r10) == SS_R10);
static_assert(offsetof(SavedState, r11) == SS_R11);
static_assert(offsetof(SavedState, r12) == SS_R12);
static_assert(offsetof(SavedState, r13) == SS_R13);
static_assert(offsetof(SavedState, r14) == SS_R14);
static_assert(offsetof(SavedState, r15) == SS_R15);
static_assert(offsetof(SavedState, rip) == SS_RIP);
static_assert(offsetof(SavedState, rsp) == SS_RSP);
static_assert(offsetof(SavedState, rflags) == SS_RFLAGS);
static_assert(offsetof(SavedState, fs_base) == SS_FS_BASE);
static_assert(offsetof(SavedState, gs_base) == SS_GS_BASE);
static_assert(offsetof(SavedState, fpu) == SS_FPU);

#endif
