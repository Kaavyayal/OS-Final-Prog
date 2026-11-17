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

#include "sched/atomic.h"

// An intrusive FIFO of T's, linked through T::next.
//   Q: Why can a node be in only one queue at a time?
//   Q: Why intrusive, rather than allocating list nodes?
template <typename T, typename LockType> class Queue {
  // only touched holding lock
  T *first = nullptr;
  T *last = nullptr;
  LockType lock{};
  Atomic<uint64_t> size_count{0};

public:
  Queue() = default;
  Queue(const Queue &) = delete;
  Queue &operator=(const Queue &) = delete;

  void add(T *t) {
    LockGuard g{lock};
    t->next = nullptr;
    if (first == nullptr) {
      first = t;
    } else {
      last->next = t;
    }
    last = t;
    size_count.add_fetch(1);
  }

  T *remove() {
    LockGuard g{lock};
    if (first == nullptr) {
      return nullptr;
    }
    auto it = first;
    first = it->next;
    if (first == nullptr) {
      last = nullptr;
    }
    size_count.sub_fetch(1);
    return it;
  }

  T *remove_all() {
    LockGuard g{lock};
    auto it = first;
    first = nullptr;
    last = nullptr;
    size_count.set(0);
    return it;
  }

  // The size, read without the lock: a hint.
  //   Q: Who uses the hint, and why is that safe?
  uint64_t size() { return size_count.get(); }
};
