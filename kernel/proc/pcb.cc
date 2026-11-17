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

#include "proc/pcb.h"

#include "mm/vmm.h"
#include "sched/spin_lock.h"
#include <utility>

PCB::PCB(uint64_t cr3, uint64_t pid, impl::VME *vmes)
    : pid(pid), cr3(cr3), vmes(vmes) {}

PCB::~PCB() {
  auto child = children.unshared();
  while (child) {auto next = child->next; delete child; child = next;}
  // nobody can use its memory any more, no TLB has it (see VMM::free_memory)
  VMM::free_memory(cr3, vmes);
  // exit_group's if there was one, its leader's otherwise (like Linux)
  std::move(exit_setter).set(exiting_ ? group_status : leader_status);
  // the process is done (see Process::create)
  impl::finished.fetch_add(1);
}
