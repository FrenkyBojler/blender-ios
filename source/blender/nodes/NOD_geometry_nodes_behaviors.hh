/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector_set.hh"

#include "NOD_geometry_nodes_bundle_fwd.hh"
#include "NOD_geometry_nodes_bundle_signature.hh"

namespace blender::nodes {

struct BehaviorDef {
  std::string type;
  BundleSignature signature;
};

struct BehaviorsDef {
  struct BehaviorDefTypeGetter {
    StringRef operator()(const BehaviorDef &def) const
    {
      return def.type;
    }
  };

  CustomIDVectorSet<BehaviorDef, BehaviorDefTypeGetter> behaviors;

  void add(BehaviorDef behavior);
};

namespace behaviors {

void foreach_behavior_in_bundle(
    const Bundle &behaviors_bundle,
    FunctionRef<void(StringRef type, const Bundle &behavior_bundle, Span<StringRef> path)> fn);

bool behavior_path_is_selected(StringRef self_path, StringRef filter, StringRef other);

}  // namespace behaviors

}  // namespace blender::nodes
