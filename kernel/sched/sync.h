/* Copyright (C) 2025-2026 Ahmed Gheith and contributors.
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

#include "lib/print.h"
#include "lib/shared.h"
#include "sched/event_loop.h"
#include "sched/spin_lock.h"
#include <coroutine>
#include <optional>
#include <utility>

// A coroutine that runs inline in its caller until it has to wait.
//   Q: How does it differ from async<T>?
//   Q: When can a sync coroutine end up on a different core than its caller?
//   Q: Why does it save heap allocations and context switches?
template <typename T> struct [[nodiscard]] sync {

  struct promise_type : PromiseEvent<promise_type> {
    SpinLock spin;
    // Set by whichever finishes first: the caller dropping its sync, or the
    // coroutine.
    //   Q: Why is one boolean enough for both events?
    //   Q: Who destroys the frame in each order?
    bool done{false};

    // The coroutine waiting for the value, if any.
    //   Q: Why can at most one coroutine be waiting?
    std::coroutine_handle<> wakeup{nullptr};

    // The value the coroutine produced.
    //   Q: When is it safe for the caller to read it?
    T value{};

    sync<T> get_return_object() { return sync<T>{this}; }

    // a sync coroutine runs in its caller's context as much as possible
    std::suspend_never initial_suspend() const noexcept { return {}; }

    struct FinalAwaiter {
      promise_type *promise;

      explicit FinalAwaiter(promise_type *p) : promise(p) {}

      bool await_ready() const noexcept {
        promise->spin.lock();
        // The caller dropped its sync: the frame can be destroyed now.
        if (promise->done) {
          promise->spin.unlock();
          return true;
        }

        // The caller still holds its sync: keep the frame for await_suspend.
        return false;
      }
      std::coroutine_handle<>
      await_suspend(std::coroutine_handle<promise_type> h) const noexcept {
        // Here: the lock is held, done is false, and the value is ready.
        ASSERT(!promise->done);

        promise->done = true;

        auto w = promise->wakeup;
        if (w) {
          // The caller is waiting for the value. It goes on here.
          promise->wakeup = nullptr;
          promise->spin.unlock();
          return w;
        } else {
          promise->spin.unlock();
          return std::noop_coroutine();
        }
      }
      void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return FinalAwaiter{this}; }

    void unhandled_exception() const noexcept {
      KPANIC("?", "Unhandled exception in sync<T>");
    }

    void return_value(const T &v) noexcept { value = v; }
  };

  // Not T *const: moving a sync nulls the old one's pointer.
  promise_type *promise;
  sync(promise_type *p) : promise(p) { ASSERT(p != nullptr); }

  ~sync() {
    // A moved-from sync must remain safely destructible.
    if (promise == nullptr)
      return;

    promise->spin.lock();
    if (promise->done == 0) {
      // Unfinished: the coroutine cleans up the promise when it finishes.
      promise->done = 1;
      promise->spin.unlock();
    } else {
      ASSERT(promise->done == 1);
      ASSERT(promise->wakeup == nullptr);
      // Finished: clean up the promise now.
      promise->done = 2;
      promise->spin.unlock();
      std::coroutine_handle<promise_type>::from_promise(*promise).destroy();
    }
  }

  // can't copy
  sync(const sync &) = delete;
  sync &operator=(const sync &) = delete;

  // can move
  sync(sync &&other) noexcept : promise(other.promise) {
    other.promise = nullptr;
  }

  bool await_ready() noexcept {
    ASSERT(promise);
    LockGuard g{promise->spin};
    return promise->done == 1;
  }

  bool await_suspend(std::coroutine_handle<> h) const noexcept {
    ASSERT(promise);
    LockGuard g{promise->spin};
    if (promise->done == 0) {
      // Not finished yet: wait for it.
      ASSERT(promise->wakeup == nullptr);
      promise->wakeup = h;
      return true;
    } else {
      return false;
    }
  }

  T await_resume() const noexcept {
    ASSERT(promise);
    return promise->value;
  }

  // The value if the coroutine is done, nothing if not.
  //   Q: When would code peek instead of co_await?
  //   Q: Which system call path uses it?
  std::optional<T> peek() const {
    ASSERT(promise);
    LockGuard g{promise->spin};
    if (promise->done == 0) {
      return std::nullopt;
    }
    return promise->value;
  }
};

// sync<T> for T = void: waits for the coroutine, no value.
//   Q: What's missing compared with sync<T>?

template <> struct [[nodiscard]] sync<void> {

  struct promise_type : PromiseEvent<promise_type> {
    SpinLock spin;
    // Set by whichever finishes first: the caller dropping its sync, or the
    // coroutine.
    //   Q: Why is one boolean enough for both events?
    //   Q: Who destroys the frame in each order?
    bool done{false};

    // The coroutine waiting for the value, if any.
    //   Q: Why can at most one coroutine be waiting?
    std::coroutine_handle<> wakeup{nullptr};

    sync<void> get_return_object() { return sync<void>{this}; }

    // a sync coroutine runs in its caller's context as much as possible
    std::suspend_never initial_suspend() const noexcept { return {}; }

    struct FinalAwaiter {
      promise_type *promise;

      explicit FinalAwaiter(promise_type *p) : promise(p) {}

      bool await_ready() const noexcept {
        promise->spin.lock();
        // The caller dropped its sync: the frame can be destroyed now.
        if (promise->done) {
          promise->spin.unlock();
          return true;
        }

        // The caller still holds its sync: keep the frame for await_suspend.
        return false;
      }
      std::coroutine_handle<>
      await_suspend(std::coroutine_handle<promise_type> h) const noexcept {
        // Here: the lock is held and done is false.
        ASSERT(!promise->done);

        promise->done = true;

        auto w = promise->wakeup;
        if (w) {
          // The caller is waiting for the value. It goes on here.
          promise->wakeup = nullptr;
          promise->spin.unlock();
          return w;
        } else {
          promise->spin.unlock();
          return std::noop_coroutine();
        }
      }
      void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return FinalAwaiter{this}; }

    void unhandled_exception() const noexcept {
      KPANIC("?", "Unhandled exception in sync<T>");
    }

    void return_void() noexcept {}
  };

  // Not T *const: moving a sync nulls the old one's pointer.
  promise_type *promise;
  sync(promise_type *p) : promise(p) { ASSERT(p != nullptr); }

  ~sync() {
    // A moved-from sync must remain safely destructible.
    if (promise == nullptr)
      return;

    promise->spin.lock();
    if (promise->done == 0) {
      // Unfinished: the coroutine cleans up the promise when it finishes.
      promise->done = 1;
      promise->spin.unlock();
    } else {
      ASSERT(promise->done == 1);
      ASSERT(promise->wakeup == nullptr);
      // Finished: clean up the promise now.
      promise->done = 2;
      promise->spin.unlock();
      std::coroutine_handle<promise_type>::from_promise(*promise).destroy();
    }
  }

  // can't copy
  sync(const sync &) = delete;
  sync &operator=(const sync &) = delete;

  // can move
  sync(sync &&other) noexcept : promise(other.promise) {
    other.promise = nullptr;
  }

  bool await_ready() noexcept {
    ASSERT(promise);
    LockGuard g{promise->spin};
    return promise->done == 1;
  }

  bool await_suspend(std::coroutine_handle<> h) const noexcept {
    ASSERT(promise);
    LockGuard g{promise->spin};
    if (promise->done == 0) {
      // Not finished yet: wait for it.
      ASSERT(promise->wakeup == nullptr);
      promise->wakeup = h;
      return true;
    } else {
      return false;
    }
  }

  void await_resume() const noexcept {}
};
