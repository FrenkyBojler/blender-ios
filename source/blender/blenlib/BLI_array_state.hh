/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "BLI_array.hh"
#include "BLI_implicit_sharing_ptr.hh"
#include "BLI_virtual_array.hh"

namespace blender {

template<typename T> class ArrayState {
 private:
  Span<T> values_;
  ImplicitSharingPtr<> sharing_info_;
  std::optional<Array<T, 0>> cached_values_;

 public:
  ArrayState() = default;

  ArrayState(const VArray<T> &values, const ImplicitSharingInfo *sharing_info)
  {
    if (values.is_span() && sharing_info) {
      values_ = values.get_internal_span();
      sharing_info->add_user();
      sharing_info_ = ImplicitSharingPtr(sharing_info);
      return;
    }
    cached_values_.emplace(values.size(), NoInitialization{});
    values.materialize_to_uninitialized(*cached_values_);
    values_ = *cached_values_;
  }

  bool is_empty() const
  {
    return values_.is_empty();
  }

  bool same_as(const VArray<T> &other_values, const ImplicitSharingInfo *other_sharing_info) const
  {
    if (sharing_info_ && other_sharing_info) {
      if (sharing_info_ == other_sharing_info) {
        /* The data is still shared.*/
        return true;
      }
    }
    if (values_.size() != other_values.size()) {
      return false;
    }
    /* Need to actually compare all elements. */
    VArraySpan<T> other_values_span(other_values);
    return values_ == other_values_span;
  }
};

}  // namespace blender
