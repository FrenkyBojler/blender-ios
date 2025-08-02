/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"

#include "NOD_geometry_nodes_bundle.hh"

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
