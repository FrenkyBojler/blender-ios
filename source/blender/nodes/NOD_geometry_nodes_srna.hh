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
  Fallback = 0,
  Value = 1,
  Attribute = 2,
  Layer = 3,
};

extern const EnumPropertyItem geometry_nodes_input_type_items_fallback[];
extern const EnumPropertyItem geometry_nodes_input_type_items_value[];
extern const EnumPropertyItem geometry_nodes_input_type_items_value_or_attribute[];
extern const EnumPropertyItem geometry_nodes_input_type_items_value_or_attribute_or_layer[];

struct GeneratedTreeSrnaData {
  ResourceScope scope;
  Vector<StructRNA *> structs;
};

StructRNA *get_geometry_nodes_interface_srna_for_modifier(const bNodeTree &tree,
                                                          GeneratedTreeSrnaData &r_generated);
StructRNA *get_geometry_nodes_interface_srna_for_operator(const bNodeTree &tree,
                                                          GeneratedTreeSrnaData &r_generated);

}  // namespace blender::nodes
