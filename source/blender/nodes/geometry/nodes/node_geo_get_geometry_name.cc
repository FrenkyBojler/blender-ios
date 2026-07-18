/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_get_geometry_name {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Geometry"_ustr).description("Geometry to retrieve the name of");
  b.add_output<decl::String>("Name"_ustr).optional_label();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry"_ustr);
  std::string name = geometry_set.name();
  if (name.empty()) {
    params.set_default_remaining_outputs();
    return;
  }
  params.set_output("Name"_ustr, std::move(name));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGetGeometryName"_ustr);
  ntype.ui_name = "Get Geometry Name";
  ntype.ui_description = "Get the name of a geometry";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_get_geometry_name
