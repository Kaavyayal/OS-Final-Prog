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

#include <type_traits>
#include <utility>

template <typename Signature> class FunctionRef;

// A non-owning reference to something callable, like a lambda.
//   Q: Why does a function whose code is in a .cc file need it?
//   Q: Why must the callable outlive the FunctionRef?
//   Q: Why doesn't it allocate, unlike std::function?
template <typename R, typename... Args> class FunctionRef<R(Args...)> {
  void *callable = nullptr;
  R (*trampoline)(void *callable, Args... args) = nullptr;

public:
  template <typename F>
    requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> &&
             std::is_invocable_r_v<R, F &, Args...>)
  // Not explicit: a lambda passes for a FunctionRef parameter as is.
  FunctionRef(F &&f) {
    using T = std::remove_reference_t<F>;
    if constexpr (std::is_function_v<T>) {
      // A function: its address (GCC and clang allow it in a void *).
      callable = reinterpret_cast<void *>(&f);
      trampoline = [](void *c, Args... args) -> R {
        return reinterpret_cast<T *>(c)(std::forward<Args>(args)...);
      };
    } else {
      callable =
          const_cast<void *>(static_cast<const void *>(__builtin_addressof(f)));
      trampoline = [](void *c, Args... args) -> R {
        return (*static_cast<T *>(c))(std::forward<Args>(args)...);
      };
    }
  }

  R operator()(Args... args) const {
    return trampoline(callable, std::forward<Args>(args)...);
  }
};
