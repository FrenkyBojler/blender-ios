/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"

#include "NOD_geometry_nodes_bundle_fwd.hh"

namespace blender::nodes {

struct HandleNestedBundleParams {
  const StringRef type;
  const Bundle &bundle;
  const Span<StringRef> path;
};

void nested_bundle_foreach(const Bundle &nested_bundle,
                           FunctionRef<void(HandleNestedBundleParams &params)> fn);

bool nested_bundle_path_is_selected(StringRef self_path, StringRef filter, StringRef other);

}  // namespace blender::nodes
