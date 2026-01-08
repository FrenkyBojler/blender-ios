/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_apply_simulated_data_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_input<decl::Bundle>("World");
  b.add_output<decl::Bundle>("World").pass_through_input_index(0).align_with_previous();
  b.add_input<decl::Bundle>("Simulated Data");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  params.set_default_remaining_outputs();
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeApplySimulatedData");
  ntype.ui_name = "Apply Sim Data";
  ntype.ui_description = "";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_apply_simulated_data_cc
