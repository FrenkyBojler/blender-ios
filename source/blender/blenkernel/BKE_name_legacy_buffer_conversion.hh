/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstddef>

#include <fmt/format.h>

#include "BLI_set.hh"

#include "DNA_listBase.h"

#include "BLI_listbase.h"

namespace blender::bke {

template<typename T, size_t S, char (T::*LegacyName)[S], char *T::*NamePtr>
inline void update_legacy_name_buffers(ListBase *values)
{
  /* Optimistically copy current names to legacy names. This is good enough in the majority of
   * cases. If there are duplicates, those will be resolved below. */
  blender::Set<blender::StringRef> truncated_names;
  bool found_duplicate_legacy_name = false;
  LISTBASE_FOREACH (T *, value, values) {
    STRNCPY_UTF8(value->*LegacyName, value->*NamePtr ? value->*NamePtr : "");
    if (!truncated_names.add(value->*LegacyName)) {
      found_duplicate_legacy_name = true;
    }
  }
  if (!found_duplicate_legacy_name) {
    return;
  }

  /* Change names that are too long to include a suffix like `.03`. This ensures that all legacy
   * names will be unique. */
  const int count = BLI_listbase_count(values);
  const int num_digits = std::to_string(count + 1).size();
  /* Add 2 because of the separator dot and null terminator. */
  const int suffix_len_with_null = num_digits + 2;
  int i = 1;
  LISTBASE_FOREACH (T *, value, values) {
    if (strlen(value->*LegacyName) > S - suffix_len_with_null) {
      size_t trimmed_old_len;
      BLI_strnlen_utf8_ex(value->*LegacyName, S - suffix_len_with_null, &trimmed_old_len);
      const std::string suffix = fmt::format(".{:0{}}", i, num_digits);
      BLI_strncpy(value->*LegacyName + trimmed_old_len, suffix.c_str(), suffix_len_with_null);
    }
    i++;
  }
}

}  // namespace blender::bke
