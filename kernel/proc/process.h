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

// The Process handle only; include pcb.h to use its fields.

#include "lib/shared.h"
#include "proc/thread.h"
#include <cstddef>
#include <cstdint>
#include <utility>

template <typename T> class async;
struct PCB;

// A process: a reference-counted handle to its PCB.
//   Q: Why is there no way to make one from a plain pointer?
//   Q: What keeps a process alive, and what happens when it's let go?
class Process {
public:
  // No process.
  Process() {}
  Process(std::nullptr_t) {}
  Process &operator=(std::nullptr_t) {
    pcb_ = nullptr;
    return *this;
  }

  // Its fields
  PCB *operator->() const { return pcb_.operator->(); }

  // The same process? Only ==: C++20 derives !=.
  bool operator==(const Process &other) const { return pcb_ == other.pcb_; }
  bool operator==(std::nullptr_t) const { return pcb_ == nullptr; }

  // The kernel's own process: the address space it runs in.
  //   Q: Why does it never start or end?
  WeakRef<PCB> weak() const {return WeakRef<PCB>{pcb_};}
  static Process promote(const WeakRef<PCB>& w) {return Process{StrongRef<PCB>{w}};}
  static Process kernel();

  // Makes a process and returns its first thread, not running yet (the
  // process is thread->process).
  //   Q: What must the caller do before the thread runs?
  //   Q: What does it return when pids run out?
  //   Q: Why must you not wait for exited while holding the process?
  static Thread create();

  // exit_group: every thread ends, and status is the process's.
  //   Q: Which call's status wins if two threads call it?
  void exit_group(int status) const;

  // For Thread: a thread ended with code.
  //   Q: When does a thread's code become the process's status?
  void thread_ended(Thread thread, int64_t code) const;

private:
  StrongRef<PCB> pcb_;

  // Wraps the first StrongRef to a new PCB.
  explicit Process(const StrongRef<PCB> &pcb);

  // The kernel's process, which never goes away.
  static const Process the_kernel;
};

// a handle is just the StrongRef
static_assert(sizeof(Process) == sizeof(StrongRef<PCB>));
