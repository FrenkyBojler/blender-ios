/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <type_traits>

namespace blender {

/**
 * #SinglePointer can be used in functions if both of the following conditions are met:
 * - The argument would otherwise be of type `void *`.
 * - The argument must never be a pointer to a pointer (hence "single pointer").
 */
struct SinglePointer {
 private:
  void *data_;

 public:
  SinglePointer(std::nullptr_t) : data_(nullptr) {}

  template<typename T> SinglePointer(T *value) : data_(value)
  {
    /** Only single pointers are allowed, but not e.g. double-pointers like `int **`. */
    static_assert(!std::is_pointer_v<std::decay_t<T>>);
  }

  operator void *() const
  {
    return data_;
  }
};

/** Same as #SinglePointer but for const pointers. */
struct ConstSinglePointer {
 private:
  const void *data_;

 public:
  ConstSinglePointer(std::nullptr_t) : data_(nullptr) {}

  template<typename T> ConstSinglePointer(T *value) : data_(value)
  {
    /** Only single pointers are allowed, but not e.g. double-pointers like `int **`. */
    static_assert(!std::is_pointer_v<std::decay_t<T>>);
  }

  operator const void *() const
  {
    return data_;
  }
};

}  // namespace blender
