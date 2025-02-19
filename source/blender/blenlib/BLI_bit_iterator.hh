/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <algorithm>

#include "BLI_bit_ref.hh"
#include "BLI_bit_span.hh"
#include "BLI_math_bits.h"

namespace blender::bits {

class SetBitIterator {
 private:
  const BitInt *data_;
  int64_t size_in_bits_;
  int64_t bit_index_;
  BitInt current_int_;

 public:
  SetBitIterator(const BitInt *data, const int64_t size_in_bits, const int64_t bit_index)
      : data_(data),
        size_in_bits_(size_in_bits),
        bit_index_(bit_index),
        current_int_(bit_index == size_in_bits ? 0 : data[bit_index >> BitToIntIndexShift])
  {
  }

  bool operator!=(const SetBitIterator &other) const
  {
    return bit_index_ != other.bit_index_;
  }

  int64_t operator*() const
  {
    return bit_index_;
  }

  SetBitIterator &operator++()
  {
    while (current_int_ == 0) {
      const int64_t next_int_index = (bit_index_ >> BitToIntIndexShift) + 1;
      const int64_t next_bit_index = next_int_index << BitToIntIndexShift;
      if (next_bit_index >= size_in_bits_) {
        bit_index_ = size_in_bits_;
        return *this;
      }
      current_int_ = data_[next_int_index];
      bit_index_ = next_bit_index;
    }
    const int next_one_index = bitscan_forward_uint64(current_int_);
    current_int_ &= ~mask_single_bit(next_one_index);
    bit_index_ = (bit_index_ & ~BitIndexMask) + next_one_index;
    bit_index_ = std::min(bit_index_, size_in_bits_);
    return *this;
  }
};

class SetBitIterable {
 private:
  BitSpan span_;

 public:
  SetBitIterable(const BitSpan span) : span_(span)
  {
    /* Other cases are not yet supported. */
    BLI_assert(span.bit_range().start() == 0);
  }

  SetBitIterator begin() const
  {
    SetBitIterator it{span_.data(), span_.size(), 0};
    if (!span_.is_empty()) {
      ++it;
    }
    return it;
  }

  SetBitIterator end() const
  {
    return SetBitIterator(span_.data(), span_.size(), span_.size());
  }
};

}  // namespace blender::bits
