/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "NOD_geometry_nodes_bundle.hh"
#include "NOD_geometry_nodes_bundle_signature.hh"
#include "NOD_node_declaration.hh"

namespace blender::nodes {

class BehaviorParseErrors {
 public:
  Vector<std::string> wrong_members;
  Vector<std::string> other_errors;

  bool has_error() const
  {
    return !this->wrong_members.is_empty();
  }
};

class BehaviorCommon {
 public:
  std::string self_path;
};

namespace behaviors {

void foreach_behavior_in_bundle(
    const Bundle &behaviors_bundle,
    FunctionRef<void(StringRef type, const Bundle &behavior_bundle, Span<StringRef> path)> fn);

bool behavior_path_is_selected(StringRef self_path, StringRef filter, StringRef other);

template<typename T>
inline void parse_member(const Bundle &bundle,
                         const StringRef name,
                         T &r_value,
                         BehaviorParseErrors &r_errors)
{
  if (const std::optional<T> value = bundle.lookup<T>(name)) {
    r_value = *value;
  }
  else {
    r_errors.wrong_members.append(name);
  }
}

}  // namespace behaviors

}  // namespace blender::nodes
