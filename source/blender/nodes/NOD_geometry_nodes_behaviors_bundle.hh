/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_string_ref.hh"

#include "NOD_geometry_nodes_bundle_fwd.hh"

namespace blender::nodes {

void foreach_behavior_in_bundle(
    const Bundle &behaviors_bundle,
    FunctionRef<void(StringRef type, const Bundle &behavior_bundle, Span<StringRef> path)> fn);

}
