/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "NOD_geometry_nodes_bundle.hh"

namespace blender::nodes {

struct HandleNestedBundleParams {
  const StringRef type;
  const Bundle &bundle;
  const Span<StringRef> path;
};

void nested_bundle_foreach(const Bundle &nested_bundle,
                           FunctionRef<void(HandleNestedBundleParams &params)> fn);

bool nested_bundle_path_is_selected(StringRef self_path, StringRef filter, StringRef other);

class BundleParseErrors {
 public:
  Vector<std::string> wrong_members;
  Vector<std::string> other_errors;

  bool has_error() const
  {
    return !this->wrong_members.is_empty();
  }
};

class NestedBundleCommon {
 public:
  std::string self_path;
};

template<typename T>
inline void bundle_parse_member(const Bundle &bundle,
                                const StringRef name,
                                T &r_value,
                                BundleParseErrors &r_errors)
{
  if (const std::optional<T> value = bundle.lookup<T>(name)) {
    r_value = *value;
  }
  else {
    r_errors.wrong_members.append(name);
  }
}

}  // namespace blender::nodes
