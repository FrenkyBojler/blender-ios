/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */
#pragma once

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender {

struct LazyStringBuilder {
  Vector<StringRef> stream;
  Vector<std::pair<int32_t, int32_t>> removals;
  size_t total_length = 0;

  /**
   * Checks if str is a contiguous continuation of the last inserted string ref.
   * If yes, merge it; else append str to the stream.
   */
  LazyStringBuilder &operator<<(StringRef str)
  {
    if (str.is_empty()) {
      return *this;
    }

    if (!stream.is_empty()) {
      StringRef &last = stream.last();
      /* Check if the memory addresses are contiguous */
      if (last.data() + last.size() == str.data()) {
        last = StringRef(last.data(), last.size() + str.size());
      }
      else {
        stream.append(str);
      }
    }
    else {
      stream.append(str);
    }

    total_length += str.size();
    return *this;
  }

  /**
   * Concatenate all strings together.
   * Memory is allocated once to fit the total length.
   */
  std::string str() const
  {
    std::string result;
    if (total_length == 0) {
      return result;
    }

    result.reserve(total_length);
    for (const auto &segment : stream) {
      result.append(segment);
    }
    BLI_assert(result.size() == total_length);
    return result;
  }

  /* Remove a range of character from the final string but keeping spaces. */
  void remove_range(int start_char, int end_char)
  {
    removals.append_as(start_char, end_char);
  }
};

}  // namespace blender
