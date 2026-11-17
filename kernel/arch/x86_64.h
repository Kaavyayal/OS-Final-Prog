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

#include "arch/machine.h"
#include "lib/print.h"
#include <cstdint>

class Features {
public:
  union {
    uint32_t id[4];
    char the_id[13];
  };
  uint32_t max_leaf;
  bool onHypervisor;
  bool hasSSE3;
  bool hasSSSE3;
  bool hasSSE4_1;
  bool hasSSE4_2;
  bool hasXSAVE;
  bool hasOSXSAVE;
  bool hasFSGSBASE;
  bool hasSMAP;
  // bool hasMonitor;
  bool hasAVX;
  // bool hasVMX;
  bool hasSVM;
  bool hasRDTSCP;
  Features();
  void dump() const;
};

template <typename T> class ControlRegister {
public:
  const T bits;
  ControlRegister(T v) : bits{v} {}
  virtual const char *bit_name(int bit) const = 0;
  virtual const char *name() const = 0;

  void dump() const {
    KPRINT("? ?\n", name(), bits);
    for (int i = 0; i < 64; i++) {
      uint64_t mask = uint64_t(1) << i;
      if (bits & mask) {
        KPRINT("  ? ?\n", i, bit_name(i));
      }
    }
  }
};

class CR0 : public ControlRegister<uint64_t> {
public:
  CR0(uint64_t v) : ControlRegister<uint64_t>{v} {}
  CR0();
  const char *name() const override;
  const char *bit_name(int bit) const override;
};

class CR4 : public ControlRegister<uint64_t> {
public:
  CR4(uint64_t v) : ControlRegister<uint64_t>{v} {}
  CR4();
  const char *name() const override;
  const char *bit_name(int bit) const override;
  static void setFSGSBASE();
  // Turns on SMAP: the kernel faults if it touches a user page.
  //   Q: When does RFLAGS.AC turn the check off?
  static void setSMAP();
};

namespace Msr {

class Msr {
public:
  const uint32_t number;
  Msr(const uint32_t number) : number{number} {}
  void write(uint64_t v) const { wrmsr(number, v); }
  uint64_t read() const { return rdmsr(number); }
};

class EFER : public Msr {
public:
  EFER() : Msr{0xC0000080} {}
};

class EOI : public Msr {
public:
  EOI() : Msr{0x80B} {}
};

class SpuriousInterruptVector : public Msr {
public:
  SpuriousInterruptVector() : Msr{0x80F} {}
};

// x2APIC ICR: one 64-bit write sends an IPI.
//   Q: Which bits pick the destination and vector?
class InterruptCommandRegister : public Msr {
public:
  InterruptCommandRegister() : Msr{0x830} {}
};

class LVTTimerRegister : public Msr {
public:
  LVTTimerRegister() : Msr{0x832} {}
};

class InitialCountRegister : public Msr {
public:
  InitialCountRegister() : Msr{0x838} {}
};

class CurrentCountRegister : public Msr {
public:
  CurrentCountRegister() : Msr{0x839} {}
};

class DivideConfigurationRegister : public Msr {
public:
  DivideConfigurationRegister() : Msr{0x83E} {}
};

// The GS base that swapgs swaps with the current one.
//   Q: What does it hold in the kernel, and in user mode?
class KernelGSBase : public Msr {
public:
  KernelGSBase() : Msr{0xC0000102} {}
};

class IA32_LSTAR : public Msr {
public:
  IA32_LSTAR() : Msr{0xC0000082} {}
};

class IA32_STAR : public Msr {
public:
  IA32_STAR() : Msr{0xC0000081} {}
};

class I32_FMASK : public Msr {
public:
  I32_FMASK() : Msr{0xC0000084} {}
};

} // namespace Msr
