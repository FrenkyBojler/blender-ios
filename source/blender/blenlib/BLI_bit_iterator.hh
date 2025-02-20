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
  int64_t offset_;

 public:
  SetBitIterator(const BitInt *data,
                 const int64_t size_in_bits,
                 const int64_t bit_index,
                 const int64_t offset)
      : data_(data),
        size_in_bits_(size_in_bits),
        bit_index_(bit_index),
        current_int_(bit_index == size_in_bits ? 0 :
                                                 data[bit_index >> BitToIntIndexShift] &
                                                     ~mask_first_n_bits(bit_index & BitIndexMask)),
        offset_(offset)
  {
  }

  friend bool operator!=(const SetBitIterator &a, const SetBitIterator &b)
  {
    return a.bit_index_ != b.bit_index_;
  }

  friend bool operator==(const SetBitIterator &a, const SetBitIterator &b)
  {
    return !(a != b);
  }

  int64_t operator*() const
  {
    return bit_index_ - offset_;
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
  SetBitIterable(const BitSpan span) : span_(span) {}

  SetBitIterator begin() const
  {
    const IndexRange bit_range = span_.bit_range();
    SetBitIterator it{
        span_.data(), bit_range.one_after_last(), bit_range.start(), bit_range.start()};
    if (!span_.is_empty()) {
      ++it;
    }
    return it;
  }

  SetBitIterator end() const
  {
    const IndexRange bit_range = span_.bit_range();
    return SetBitIterator(
        span_.data(), bit_range.one_after_last(), bit_range.one_after_last(), bit_range.start());
  }
};

}  // namespace blender::bits
