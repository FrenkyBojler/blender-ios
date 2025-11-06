/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_lsystem_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Geometry>("Geometry");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_default_remaining_outputs();
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeLSystem");
  ntype.ui_name = "L-System";
  ntype.ui_description = "Create geometry using a L-System";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_lsystem_cc
