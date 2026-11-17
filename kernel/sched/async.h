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

#include "lib/shared.h"
#include "sched/atomic.h"
#include "sched/event_loop.h"
#include "sched/spin_lock.h"
#include "sched/sync.h"
#include <coroutine>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace impl {

// An awaiter that puts the caller back on the ready queue.
struct YieldAwaiter {
  bool await_ready() const noexcept { return false; }
  template <Queueable P>
  void await_suspend(std::coroutine_handle<P> h) const noexcept {
    ready_queue.add(&h.promise());
  }
  void await_resume() const noexcept {}
};
} // namespace impl

// Suspends the caller and puts it back on the ready queue.
//   Q: When, and on which core, does the caller run again?
[[nodiscard]]
inline impl::YieldAwaiter yield() noexcept {
  return {};
}

template <typename T> class async_setter;

// A coroutine that runs on its own from the event loop.
//   Q: Why does the caller go on before the coroutine starts?
//   Q: How many times can its value be awaited, and from where?
//   Q: How does it relate to async_setter?
template <typename T = void> class async {

  // The value and its waiters, shared by every async<T> for it and its
  // setter: the last one to go deletes it.
  //   Q: Why can't it live in the coroutine's frame?
  //   Q: Why does it need a count of its own instead of a StrongRef?
  struct State {
    Atomic<uint64_t> refs{1};
    SpinLock spin{};
    bool ready = false;
    T value{};
    Queue<Event, NoLock> waiters{}; // the coroutines waiting for it
  };

  State *state = nullptr;

  static State *add_ref(State *s) {
    if (s != nullptr) {
      s->refs.fetch_add(1);
    }
    return s;
  }

  static void drop(State *s) {
    if (s != nullptr && s->refs.sub_fetch(1) == 0) {
      delete s;
    }
  }

  // Sets the value and resumes every waiter (async_setter::set).
  //   Q: What keeps it from being set twice?
  //   Q: Why read a waiter's next before queueing it?
  [[gnu::noinline]] static void set(State &s, const T &value) {
    LockGuard g{s.spin};
    ASSERT(!s.ready);
    s.value = value;
    s.ready = true;
    Event *event = s.waiters.remove_all();
    while (event != nullptr) {
      // next first: once it's queued, it can run and be gone
      Event *const next = event->next;
      impl::ready_queue.add(event);
      event = next;
    }
  }

  // Takes over a reference to s (async_setter::result).
  explicit async(State *s) : state(s) {}
  friend class async_setter<T>;

public:
  async() {}
  async(const async &other) : state(add_ref(other.state)) {}
  async(async &&other) noexcept : state(std::exchange(other.state, nullptr)) {}
  // other is a copy (or moved in): its destructor lets go of the old state
  async &operator=(async other) noexcept {
    std::swap(state, other.state);
    return *this;
  }
  ~async() { drop(state); }

  // True if it refers to no computation (e.g. default constructed).
  bool operator==(std::nullptr_t) const { return state == nullptr; }

  struct promise_type : PromiseEvent<promise_type> {

    // co_return sets it
    async_setter<T> setter{};

    promise_type() { impl::started.fetch_add(1); }

    ~promise_type() { impl::finished.fetch_add(1); }

    // starts later, from the event loop: the caller goes on now
    impl::YieldAwaiter initial_suspend() const noexcept { return {}; }
    async<T> get_return_object() noexcept { return setter.result(); }
    void return_value(const T &v) noexcept { std::move(setter).set(v); }
    void unhandled_exception() const noexcept;
    std::suspend_never final_suspend() const noexcept { return {}; }
  };

  // co_await on an async: waits for the value and gives it.
  //   Q: What if the value is already set?
  //   Q: What keeps the State alive while the caller waits?
  class Wait {
    State &s;

  public:
    explicit Wait(State &s) : s(s) {}

    bool await_ready() {
      LockGuard g{s.spin};
      return s.ready;
    }

    template <Queueable P> bool await_suspend(std::coroutine_handle<P> h) {
      LockGuard g{s.spin};
      if (s.ready) {
        return false;
      }
      s.waiters.add(&h.promise());
      return true;
    }

    T &await_resume() { return s.value; }
  };

  Wait operator co_await() const noexcept {
    ASSERT(state != nullptr);
    return Wait{*state};
  }

  // The value if it's set already, nothing if not.
  //   Q: Who needs to look without waiting?
  std::optional<T> peek() const {
    ASSERT(state != nullptr);
    LockGuard g{state->spin};
    if (!state->ready) {
      return std::nullopt;
    }
    return state->value;
  }
};

// The writing side of an async<T>: sets its value, once.
//   Q: How does the type keep a second set from happening?
//   Q: Who wakes up when it's set?
template <typename T> class async_setter {
  using A = async<T>;
  typename A::State *state;

public:
  async_setter() : state(new typename A::State{}) {}

  async_setter(const async_setter &) = delete;
  async_setter &operator=(const async_setter &) = delete;
  async_setter(async_setter &&other) noexcept
      : state(std::exchange(other.state, nullptr)) {}
  async_setter &operator=(async_setter &&other) noexcept {
    std::swap(state, other.state);
    return *this;
  }
  ~async_setter() { A::drop(state); }

  // A reading side of the value; make as many as you like.
  A result() const {
    ASSERT(state != nullptr); // not set yet
    return A{A::add_ref(state)};
  }

  // Sets the value and wakes whoever waits for it.
  //   Q: Why can only an rvalue setter call it?
  void set(const T &value) && {
    ASSERT(state != nullptr); // once
    typename A::State *const s = std::exchange(state, nullptr);
    A::set(*s, value);
    A::drop(s);
  }
};

// async<void>: runs on its own, and nobody waits for it.
//   Q: Why can't the caller co_await it?
template <> class async<void> {
public:
  struct promise_type : PromiseEvent<promise_type> {

    promise_type() { impl::started.fetch_add(1); }

    ~promise_type() { impl::finished.fetch_add(1); }

    // starts later, from the event loop: the caller goes on now
    impl::YieldAwaiter initial_suspend() const noexcept { return {}; }
    async<void> get_return_object() noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() const noexcept;
    std::suspend_never final_suspend() const noexcept { return {}; }
  };

  async() {}
};
