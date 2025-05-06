/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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
    static_assert(!std::is_pointer_v<std::decay_t<T>>);
  }

  void *data() const
  {
    return data_;
  }
};

}  // namespace blender
