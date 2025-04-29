/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_resource_scope.hh"

struct StructRNA;
struct bNodeTree;

namespace blender::nodes {

enum class GeometryNodesInputTypeFloat {
  Value = 0,
  Attribute = 1,
};

enum class GeometryNodesInputTypeInt {
  Value = 0,
  Attribute = 1,
};

struct GeneratedTreeSrnaData {
  ResourceScope scope;
  Vector<StructRNA *> structs;
};

StructRNA *get_geometry_nodes_inputs_srna(const bNodeTree &tree,
                                          GeneratedTreeSrnaData &r_generated);

}  // namespace blender::nodes
