/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"

#include <ostream>

namespace blender {

std::ostream &operator<<(std::ostream &stream, StringRef ref)
{
  stream << std::string(ref);
  return stream;
}

std::ostream &operator<<(std::ostream &stream, StringRefNull ref)
{
  stream << std::string(ref.data(), size_t(ref.size()));
  return stream;
}

void StringRefBase::copy_utf8_truncated(char *dst, const int64_t dst_size) const
{
  /* Destination must at least hold the null terminator. */
  BLI_assert(dst_size >= 1);
  /* The current #StringRef is assumed to contain valid UTF-8. */
  BLI_assert(BLI_str_utf8_invalid_byte(data_, size_) == -1);

  /* Common case when the string can just be copied over entirely. */
  if (size_ < dst_size) {
    this->copy_unsafe(dst);
    return;
  }

  /* Make a temporary copy because we need the null terminator to use #BLI_strncpy_utf8. Should be
   * fine performance-wise, because it's rare that the truncation is actually used. */
  const std::string str_copy(data_, size_);
  BLI_strncpy_utf8(dst, str_copy.c_str(), dst_size);
}

}  // namespace blender
