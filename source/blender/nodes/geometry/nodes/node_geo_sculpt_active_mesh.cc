/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_sculpt_active_mesh_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Geometry>("Active Mesh").description("Active Mesh");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const Object *object = params.self_object();

  if (object && object->type == OB_MESH) {
    const Mesh *mesh = static_cast<const Mesh *>(object->data);

    if (mesh) {
      GeometrySet geometry_set;
      geometry_set.replace_mesh(const_cast<Mesh *>(mesh), bke::GeometryOwnershipType::ReadOnly);

      params.set_output("Active Mesh", std::move(geometry_set));
      return;
    }
  }

  params.set_default_remaining_outputs();
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeSculptActiveMesh", GEO_NODE_SCULPT_ACTIVE_MESH, NODE_CLASS_INPUT);
  ntype.ui_name = "Active Mesh";
  ntype.ui_description = "Active Mesh";
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_sculpt_active_mesh_cc
