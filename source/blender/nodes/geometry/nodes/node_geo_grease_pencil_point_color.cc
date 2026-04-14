/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_fields.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_grease_pencil_point_color__cc {

static void node_declare(NodeDeclarationBuilder &b)
{  
  b.add_output<decl::Color>("Vertex Color"_ustr).field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<ColorGeometry4f> vertex_color = AttributeFieldInput::get_field<ColorGeometry4f, "vertex_color">();
  params.set_output("Vertex Color"_ustr, std::move(vertex_color));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGreasePencilPointColor");
  ntype.ui_name = "Point Color";
  ntype.ui_description = "Retrieve the color of each point in Grease Pencil strokes";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grease_pencil_point_color__cc
