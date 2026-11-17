#pragma once

#include "lib/print.h"
#include "sched/atomic.h"
#include <cstdint>

template <typename T> class WeakRef;
template <typename T> class UniqueRef;

namespace impl {

// The shared counts behind StrongRef and WeakRef.
//   Q: When is the object deleted, and when is the Middle?
//   Q: Why do all StrongRefs together count as one weak reference?
template <typename T> class Middle {
  // Deletes the object: compiled where a Middle is made, from a new T.
  static void delete_object(T *p) { delete p; }

public:
  T *ptr;
  // How to delete it, picked while T is complete
  //   Q: Why does a plain delete in remove_strong need T complete everywhere a
  //      StrongRef<T> (or a coroutine holding one) is destroyed?
  void (*const destroy)(T *);
  Atomic<uint64_t> strong;
  Atomic<uint64_t> weak;

  Middle(T *ptr) : ptr(ptr), destroy(delete_object), strong(1), weak(1) {}

  ~Middle() {}

  // Another StrongRef, from one the caller holds.
  static Middle *add_strong(Middle *m) {
    if (m != nullptr) {
      m->strong.fetch_add(1);
    }
    return m;
  }

  // A StrongRef from a WeakRef: nullptr once the object is gone.
  //   Q: Why can't promote just increment strong?
  static Middle *promote(Middle *m) {
    if (m == nullptr) {
      return nullptr;
    }
    uint64_t s = m->strong.get();
    while (s != 0) {
      if (m->strong.compare_exchange(s, s + 1)) {
        return m;
      }
    }
    return nullptr;
  }

  static void remove_strong(Middle *m) {
    if (m == nullptr) {
      return;
    }
    const uint64_t s = m->strong.fetch_sub(1);
    ASSERT(s > 0);
    if (s == 1) {
      // the last StrongRef: nobody else can see ptr
      T *const del_ptr = m->ptr;
      m->ptr = nullptr;
      m->destroy(del_ptr);
      // the StrongRefs' share of weak
      remove_weak(m);
    }
  }

  // Another WeakRef, from a reference the caller holds (maybe expired).
  static Middle *add_weak(Middle *m) {
    if (m != nullptr) {
      m->weak.fetch_add(1);
    }
    return m;
  }

  static Middle *remove_weak(Middle *m) {
    if (m == nullptr) {
      return nullptr;
    }
    const uint64_t w = m->weak.fetch_sub(1);
    ASSERT(w > 0);
    if (w == 1) {
      delete m;
      return nullptr;
    }
    return m;
  }
};
} // namespace impl

template <typename T> class StrongRef {
  using M = impl::Middle<T>;

  mutable M *middle;

  inline M *add_strong(M *m) { return M::add_strong(m); }
  inline M *promote(M *m) { return M::promote(m); }
  inline void remove_strong(M *m) { M::remove_strong(m); }

  inline M *mm(T *ptr) {
    if (ptr == nullptr) {
      return nullptr;
    } else {
      return new M(ptr);
    }
  }

public:
  // Constructors
  StrongRef() : middle(nullptr) {}
  explicit StrongRef(T *ptr) : middle(mm(ptr)) {}
  StrongRef(const StrongRef<T> &other) : middle(add_strong(other.middle)) {}
  StrongRef(StrongRef<T> &&other) : middle(other.middle) {
    other.middle = nullptr;
  }
  StrongRef(const WeakRef<T> &rhs) : middle(promote(rhs.middle)) {}
  StrongRef(const UniqueRef<T> &rhs) : middle(mm(rhs.ptr)) {
    rhs.ptr = nullptr;
  }

  // Destructor
  ~StrongRef() { remove_strong(middle); }

  // dereference
  T *operator->() const {
    ASSERT(middle != nullptr);
    ASSERT(middle->ptr != nullptr);
    return middle->ptr;
  }

  // Only ==: C++20 derives the rest.
  bool operator==(const StrongRef<T> &other) const {
    return middle == other.middle;
  }
  bool operator==(std::nullptr_t) const {
    return (middle == nullptr) || (middle->ptr == nullptr);
  }
  bool operator==(const T *ptr) const {
    if (middle == nullptr) {
      return ptr == nullptr;
    } else {
      return (middle->ptr == ptr);
    }
  }

  StrongRef &operator=(std::nullptr_t) {
    M *old = middle;
    middle = nullptr;
    remove_strong(old);
    return *this;
  }
  StrongRef &operator=(const WeakRef<T> &rhs) {
    M *m = promote(rhs.middle);
    M *old = middle;
    middle = m;
    remove_strong(old);
    return *this;
  }
  StrongRef &operator=(const UniqueRef<T> &rhs) {
    M *m = mm(rhs.ptr);
    rhs.ptr = nullptr;
    M *old = middle;
    middle = m;
    remove_strong(old);
    return *this;
  }
  StrongRef &operator=(const StrongRef<T> &other) {
    M *m = add_strong(other.middle);
    M *old = middle;
    middle = m;
    remove_strong(old);
    return *this;
  }
  StrongRef &operator=(StrongRef<T> &&other) {
    if (this != &other) {
      M *old = middle;
      middle = other.middle;
      other.middle = nullptr;
      remove_strong(old);
    }
    return *this;
  }

  // factory
  template <typename... Args> static StrongRef<T> make(Args... args) {
    return StrongRef<T>{new T(args...)};
  }

  friend class WeakRef<T>;
};

template <typename T> class WeakRef {

  using M = impl::Middle<T>;

  M *middle;

  inline M *add_weak(M *m) { return M::add_weak(m); }

  inline M *remove_weak(M *m) { return M::remove_weak(m); }

public:
  // Constructors
  WeakRef() : middle(nullptr) {}

  explicit WeakRef(T *ptr) : middle(nullptr) {
    if (ptr != nullptr) {
      delete ptr;
    }
  }

  WeakRef(const WeakRef<T> &other) : middle(add_weak(other.middle)) {}
  WeakRef(WeakRef<T> &&other) : middle(other.middle) { other.middle = nullptr; }
  WeakRef(const StrongRef<T> &other) : middle(add_weak(other.middle)) {}
  WeakRef(const UniqueRef<T> &rhs) : middle(nullptr) {
    if (rhs.ptr != nullptr) {
      delete rhs.ptr;
      rhs.ptr = nullptr;
    }
  }

  // Destructor
  ~WeakRef() { remove_weak(middle); }

  // Assignment operators
  // Like StrongRef's: store the new one, then drop the old one
  WeakRef &operator=(std::nullptr_t) {
    M *old = middle;
    middle = nullptr;
    remove_weak(old);
    return *this;
  }
  WeakRef &operator=(const WeakRef<T> &other) {
    if (middle != other.middle) {
      M *old = middle;
      middle = add_weak(other.middle);
      remove_weak(old);
    }
    return *this;
  }
  WeakRef &operator=(WeakRef<T> &&other) {
    if (this != &other) {
      M *old = middle;
      middle = other.middle;
      other.middle = nullptr;
      remove_weak(old);
    }
    return *this;
  }
  WeakRef &operator=(const UniqueRef<T> &rhs) {
    M *old = middle;
    middle = nullptr;
    remove_weak(old);
    if (rhs.ptr != nullptr) {
      delete rhs.ptr;
      rhs.ptr = nullptr;
    }
    return *this;
  }
  WeakRef &operator=(const StrongRef<T> &other) {
    if (middle != other.middle) {
      M *old = middle;
      middle = add_weak(other.middle);
      remove_weak(old);
    }
    return *this;
  }

  // factory
  template <typename... Args> static WeakRef<T> make(Args... args) {
    return WeakRef<T>{new T(args...)};
  }

  // friends
  friend class StrongRef<T>;
};

template <typename T> class UniqueRef {
  mutable T *ptr;

  inline void replace(T *new_ptr) {
    if (ptr != nullptr) {
      delete ptr;
    }
    ptr = new_ptr;
  }

  friend class StrongRef<T>;
  friend class WeakRef<T>;

public:
  // Constructors
  UniqueRef() : ptr(nullptr) {}
  explicit UniqueRef(T *ptr) : ptr(ptr) {}
  UniqueRef(const UniqueRef<T> &) = delete;
  UniqueRef(UniqueRef<T> &&other) : ptr(other.ptr) { other.ptr = nullptr; }
  UniqueRef(const StrongRef<T> &) : ptr(nullptr) {}
  UniqueRef(const WeakRef<T> &) : ptr(nullptr) {}

  // Destructor
  ~UniqueRef() {
    if (ptr != nullptr) {
      delete ptr;
    }
  }

  // dereference
  T *operator->() const {
    ASSERT(ptr != nullptr);
    return ptr;
  }

  // Assignment operators
  UniqueRef &operator=(std::nullptr_t) {
    replace(nullptr);
    return *this;
  }
  UniqueRef &operator=(const UniqueRef<T> &) = delete;
  UniqueRef &operator=(UniqueRef<T> &&other) {
    if (this != &other) {
      T *const p = other.ptr;
      other.ptr = nullptr;
      replace(p);
    }
    return *this;
  }
  UniqueRef &operator=(const StrongRef<T> &) = delete;
  UniqueRef &operator=(const WeakRef<T> &) = delete;

  // Only ==: C++20 derives != and the reversed forms
  bool operator==(std::nullptr_t) const { return ptr == nullptr; }
  bool operator==(const UniqueRef<T> &rhs) const { return ptr == rhs.ptr; }

  // factory
  template <typename... Args> static UniqueRef<T> make(Args... args) {
    return UniqueRef<T>{new T(args...)};
  }
};