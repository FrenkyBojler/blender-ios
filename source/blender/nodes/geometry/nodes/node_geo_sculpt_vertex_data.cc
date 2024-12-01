/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"
using namespace blender::bke;

namespace blender::nodes::node_geo_sculpt_vertex_data_cc {

  static void node_declare(NodeDeclarationBuilder& b)
  {
    b.add_output<decl::Vector>("Position").field_source().description("Position");
    b.add_output<decl::Vector>("Normal").field_source().description("Normal");
    b.add_output<decl::Int>("Index").field_source().description("Index");
  }

  static void node_geo_exec(GeoNodeExecParams params)
  {
    /*Field<float3> position_field{
      std::make_shared<SculptingFieldInput>("position", CPPType::get<float3>()) };

    Field<float3> normal_field{
        std::make_shared<SculptingFieldInput>("normal", CPPType::get<float3>()) };

    Field<float3> index_field{
        std::make_shared<SculptingFieldInput>("index", CPPType::get<int>()) };

    params.set_output("Position", std::move(position_field));
    params.set_output("Normal", std::move(normal_field));
    params.set_output("Index", std::move(index_field)); */
    params.set_default_remaining_outputs();
  }

  static void node_register()
  {
    static blender::bke::bNodeType ntype;
    geo_node_type_base(&ntype, GEO_NODE_SCULPT_VERTEX_DATA, "Vertex Data", NODE_CLASS_INPUT);
    ntype.declare = node_declare;
    ntype.geometry_node_execute = node_geo_exec;
    blender::bke::node_register_type(&ntype);
  }
  NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sculpt_vertex_data_cc
