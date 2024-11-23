/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_stroke_plane_cc {

  static void node_declare(NodeDeclarationBuilder& b)
  {
    b.add_output<decl::Vector>("Plane Normal").description("Plane Normal");
    b.add_output<decl::Vector>("Plane Origin").description("Plane Origin");
    b.add_output<decl::Bool>("Is First Step").description("Is First Step");
    b.add_output<decl::Matrix>("Local Transform").description("Local Transform");
  }

  static void node_geo_exec(GeoNodeExecParams params)
  {
    const nodes::GeoNodesCallData* call_data = params.user_data()->call_data;

    if (call_data && call_data->sculpting_data) {
      const float3 plane_normal = call_data->sculpting_data->plane_normal;
      const float3 plane_origin = call_data->sculpting_data->plane_origin;
      const bool is_first_step = call_data->sculpting_data->is_first_step;
      const float4x4 &local_transform = call_data->sculpting_data->local_transform;
      params.set_output("Plane Normal", plane_normal);
      params.set_output("Plane Origin", plane_origin);
      params.set_output("Is First Step", is_first_step);
      params.set_output("Local Transform", local_transform);
    }
    else {
      params.set_default_remaining_outputs();
    }
  }

  static void node_register()
  {
    static blender::bke::bNodeType ntype;
    geo_node_type_base(&ntype, GEO_NODE_SCULPT_STROKE_INFO, "Stroke Info", NODE_CLASS_INPUT);
    ntype.declare = node_declare;
    ntype.geometry_node_execute = node_geo_exec;
    blender::bke::node_register_type(&ntype);
  }
  NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_stroke_info_cc
