/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "NOD_nodes_srna.hh"

#include "RNA_types.hh"

namespace blender {

struct StructRNA;
struct bNodeTree;

namespace nodes {

enum class CompositorNodesInputType {
  Fallback = 0,
  Value = 1,
};

extern const EnumPropertyItem compositor_nodes_input_type_items_fallback[];
extern const EnumPropertyItem compositor_nodes_input_type_items_value[];

StructRNA *get_compositor_nodes_interface_srna_for_strip_modifier(
    const bNodeTree &tree, GeneratedTreeSrnaData &r_generated);

}  // namespace nodes
}  // namespace blender
