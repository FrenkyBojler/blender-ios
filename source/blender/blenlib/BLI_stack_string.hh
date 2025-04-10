/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender {

template<int64_t InlineBufferCapacity = 1024> class StackString {
 private:
  Vector<char, InlineBufferCapacity> data_ = {'\0'};

 public:
  StackString() = default;

  StackString(StringRef str)
  {
    data_.resize(str.size() + 1);
    memcpy(str.data(), data_.data(), str.size());
    data_.last() = '\0';
  }

  char *ensure_size(const int64_t size)
  {
    data_.resize(size);
    return data_.data();
  }

  operator StringRefNull() const
  {
    BLI_assert(this->is_null_terminated());
    return data_.data();
  }

  const char *c_str() const
  {
    BLI_assert(this->is_null_terminated());
    return data_.data();
  }

  char *c_str()
  {
    BLI_assert(this->is_null_terminated());
    return data_.data();
  }

 private:
  bool is_null_terminated() const
  {
    return data_.contains('\0');
  }
};

}  // namespace blender
