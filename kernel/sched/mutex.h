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

#include "sched/event_loop.h"
#include "sched/spin_lock.h"

// A lock around a value of type T, reachable only through co_await.
//   Q: How does the Proof keep code from touching the value unlocked?
//   Q: Who gets the lock when the Proof is destroyed?

template <typename T> class Mutex {

  SpinLock spin_lock{};
  bool locked = false;
  Queue<Event, NoLock> waiters{}; // the coroutines waiting for it
  T data;

  void unlock() {
    LockGuard g{spin_lock};
    ASSERT(locked);
    // the next waiter gets it (locked stays true)
    Event *next = waiters.remove();

    if (next != nullptr) {
      impl::ready_queue.add(next);
    } else {
      locked = false;
    }
  }

public:
  class [[nodiscard]] Proof {
    Mutex *the_lock;
    Proof(Mutex *l) : the_lock(l) {}

    friend class ::Mutex<T>;

  public:
    // can't copy
    Proof(const Proof &) = delete;
    Proof &operator=(const Proof &) = delete;

    // can move
    Proof(Proof &&other) : the_lock(other.the_lock) {
      other.the_lock = nullptr;
    }

    Proof &operator=(Proof &&other) {
      if (this != &other) {
        unlock();
        the_lock = other.the_lock;
        other.the_lock = nullptr;
      }
      return *this;
    }

    ~Proof() { unlock(); }

    // The protected value.
    T &data() {
      ASSERT(the_lock != nullptr);
      return the_lock->data;
    }

    // Unlocks before the Proof goes away.
    //   Q: What happens if data() is called after?
    void unlock() {
      if (the_lock != nullptr) {
        the_lock->unlock();
        the_lock = nullptr;
      }
    }
  };

  explicit Mutex(const T &initial_value = {}) : data(initial_value) {}

  // The value without the lock, once nobody else can have the mutex.
  //   Q: What does it check, and when is that true?
  T &unshared() {
    ASSERT(!locked);
    return data;
  }

  // co_await mutex: waits for the lock and gives a Proof.
  //   Q: Why does await_suspend check locked again?
  class Lock {
    Mutex &mutex;

  public:
    explicit Lock(Mutex &mutex) : mutex(mutex) {}

    bool await_ready() {
      LockGuard g{mutex.spin_lock};
      if (mutex.locked) [[unlikely]] {
        return false;
      } else {
        mutex.locked = true;
        return true;
      }
    }

    template <Queueable P> bool await_suspend(std::coroutine_handle<P> h) {
      LockGuard g{mutex.spin_lock};
      if (mutex.locked) [[unlikely]] {
        mutex.waiters.add(&h.promise());
        return true;
      } else {
        mutex.locked = true;
        return false;
      }
    }

    Proof await_resume() { return Proof{&mutex}; }
  };

  Lock operator co_await() { return Lock{*this}; }
};