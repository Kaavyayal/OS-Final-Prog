#include "arch/idt.h"
#include "arch/gdt.h"
#include "arch/machine.h"
#include "arch/x86_64.h"
#include <cstdint>

namespace impl {

constexpr uint8_t spurious_index = 50;

class [[gnu::packed]] IdtEntry {
public:
  uint32_t data[4];

  void set(uint64_t offset, uint8_t type, uint8_t dpl) {
    data[0] = (uint32_t(GDT::KERNEL_CS) << 16) | uint16_t(offset);
    data[1] = uint32_t(uint16_t(offset >> 16)) << 16 | 0x8000 | (dpl << 13) |
              (type << 8);
    data[2] = uint32_t(offset >> 32);
    data[3] = 0;
  }
};

struct [[gnu::packed]] Idtr {
  uint16_t size;
  uint64_t base;
};

static_assert(sizeof(Idtr) == 10);

static_assert(sizeof(IdtEntry) == 16);
static_assert(sizeof(uintptr_t) == 8);

IdtEntry idt[256];
Idtr idtr{sizeof(idt) - 1, (uintptr_t)idt};
} // namespace impl

extern "C" void generalProtectionHandler_();
void IDT::init_system() {
  IDT::interrupt(13, uintptr_t(generalProtectionHandler_));
  // init_core points the APIC at spurious_index
  IDT::interrupt(impl::spurious_index, (uintptr_t)spuriousHandler_);

  // Mask the legacy PIC: its interrupts would land on exception vectors.
  outb(0x21, 0xff);
  outb(0xA1, 0xff);
}
void IDT::init_core(void) {
  // Mask the local APIC's sources: the firmware's have no handlers.
  for (uint32_t msr = 0x832; msr <= 0x837; msr++) {
    wrmsr(msr, rdmsr(msr) | (1 << 16));
  }
  Msr::SpuriousInterruptVector().write((1 << 8) | impl::spurious_index);
  // KPRINT("suporious interrupt vector = ?\n",
  //        Msr::SpuriousInterruptVector().read());
  asm volatile("lidt %0" : : "m"(impl::idtr));
}

void IDT::interrupt(uint32_t index, uintptr_t handler) {
  using namespace impl;

  idt[index].set(handler, 0xe, 0);
}

void IDT::trap(uint32_t index, uintptr_t handler, uint32_t dpl) {
  using namespace impl;

  idt[index].set(handler, 0xf, dpl);
}
