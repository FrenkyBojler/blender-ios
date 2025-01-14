/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sculpt_stroke_plane_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Vector>("Cursor Location").description("Cursor Location");
  b.add_output<decl::Vector>("Plane Normal").description("Plane Normal");
  b.add_output<decl::Vector>("Plane Center").description("Plane Center");
  b.add_output<decl::Bool>("Is First Step").description("Is First Step");
  b.add_output<decl::Int>("Step").description("Step");
  b.add_output<decl::Matrix>("Local Transform").description("Local Transform");
  b.add_output<decl::Matrix>("Texture Transform").description("Texture Transform");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const nodes::GeoNodesCallData *call_data = params.user_data()->call_data;

  if (call_data && call_data->sculpt_data) {
    const float3 cursor_location = call_data->sculpt_data->cursor_location;
    const float3 plane_normal = call_data->sculpt_data->plane_normal;
    const float3 plane_center = call_data->sculpt_data->plane_center;
    const bool is_first_step = call_data->sculpt_data->is_first_step;
    const int step = call_data->sculpt_data->step;
    const float4x4 &local_transform = call_data->sculpt_data->local_transform;
    const float4x4 &texture_transform = call_data->sculpt_data->texture_transform;

    params.set_output("Cursor Location", cursor_location);
    params.set_output("Plane Normal", plane_normal);
    params.set_output("Plane Center", plane_center);
    params.set_output("Is First Step", is_first_step);
    params.set_output("Step", step);
    params.set_output("Local Transform", local_transform);
    params.set_output("Texture Transform", texture_transform);
  }
  else {
    params.set_default_remaining_outputs();
  }
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSculptStrokeInfo");
  ntype.ui_name = "Stroke Info";
  ntype.ui_description = "Stroke Info";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sculpt_stroke_plane_cc
