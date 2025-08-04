/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_resource_scope.hh"

#include "RNA_types.hh"

struct StructRNA;
struct bNodeTree;

namespace blender::nodes {

/**
 * This is share across all socket types, even though some entries don't make sense for some types.
 */
enum class GeometryNodesInputType {
  Value = 0,
  Attribute = 1,
};

extern const EnumPropertyItem geometry_nodes_input_type_items[];

struct GeneratedTreeSrnaData {
  ResourceScope scope;
  Vector<StructRNA *> structs;
  Map<StringRef, StructRNA *> inputs_map;
};

StructRNA *get_geometry_nodes_interface_srna(const bNodeTree &tree,
                                             GeneratedTreeSrnaData &r_generated);

}  // namespace blender::nodes
