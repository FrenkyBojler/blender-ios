/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sculpt_brush_info_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Strength").description("Strength");
  b.add_output<decl::Float>("Radius").description("Radius");
  b.add_output<decl::Color>("Color").description("Color");
  b.add_output<decl::Float>("Flip").description("Flip");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const nodes::GeoNodesCallData *call_data = params.user_data()->call_data;

  if (call_data && call_data->sculpt_data) {
    const float strength = call_data->sculpt_data->strength;
    const float radius = call_data->sculpt_data->radius;
    const float4 color = call_data->sculpt_data->color;
    const bool flip = call_data->sculpt_data->flip;
    params.set_output("Strength", strength);
    params.set_output("Radius", radius);
    params.set_output("Color", ColorGeometry4f(color));
    params.set_output("Flip", flip);
  }
  else {
    params.set_default_remaining_outputs();
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSculptBrushInfo", GEO_NODE_SCULPT_BRUSH_INFO, NODE_CLASS_INPUT);
  ntype.ui_name = "Brush Info";
  ntype.ui_description = "Brush Info";
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sculpt_brush_info_cc
