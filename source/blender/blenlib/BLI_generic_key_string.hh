/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_generic_key.hh"
#include "BLI_string_ref.hh"
#include "BLI_struct_equality_utils.hh"

namespace blender {

class GenericStringKey : public GenericKey {
 private:
  std::string value_;
  StringRef value_ref_;

 public:
  GenericStringKey(StringRef value) : value_ref_(value) {}

  uint64_t hash() const override
  {
    return get_default_hash(value_ref_);
  }

  BLI_STRUCT_EQUALITY_OPERATORS_1(GenericStringKey, value_ref_)

  bool equal_to(const GenericKey &other) const override
  {
    if (const auto *other_typed = dynamic_cast<const GenericStringKey *>(&other)) {
      return value_ref_ == other_typed->value_ref_;
    }
    return false;
  }

  std::unique_ptr<GenericKey> to_storable() const override
  {
    auto storable_key = std::make_unique<GenericStringKey>("");
    storable_key->value_ = value_ref_;
    storable_key->value_ref_ = storable_key->value_;
    return storable_key;
  }
};

}  // namespace blender
