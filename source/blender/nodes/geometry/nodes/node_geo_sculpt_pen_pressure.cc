/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sculpt_pen_pressure_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Pen Pressure").description("Pen Pressure");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const nodes::GeoNodesCallData *call_data = params.user_data()->call_data;

  if (call_data && call_data->sculpt_data) {
    const float pen_pressure = call_data->sculpt_data->pen_pressure;
    params.set_output("Pen Pressure", pen_pressure);
  }
  else {
    params.set_default_remaining_outputs();
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSculptPenPressure");
  ntype.ui_name = "Pen Pressure";
  ntype.ui_description = "Pen Pressure";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sculpt_pen_pressure_cc
