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

#include <concepts>
#include <coroutine>

// Something for the event loop to do.
//   Q: What are the two kinds of Event?
//   Q: Why does an Event carry its own next link?
struct Event {
  // the link of the queue it's in (one at a time)
  Event *next = nullptr;

  // Does the work, once.
  //   Q: Why might it never return?
  //   Q: Why can't it touch *this once the work starts?
  virtual void dispatch() = 0;

protected:
  ~Event() = default;
};

// A coroutine's promise, as its own event: dispatch resumes the coroutine.
//   Q: Why doesn't kernel code care which thread or CR3 the core has?
//   Q: Why does each coroutine need only one event?
//   Q: How does dispatch find the coroutine without storing its handle?
template <typename P> struct PromiseEvent : Event {
  // (the coroutine destroys *this once it goes on)
  void dispatch() override {
    std::coroutine_handle<P>::from_promise(static_cast<P &>(*this)).resume();
  }
};

// A promise that is its own event: what a waiting primitive can queue.
//   Q: Which coroutines can wait on an async, a Mutex or a Barrier?
template <typename P>
concept Queueable = std::derived_from<P, PromiseEvent<P>>;
