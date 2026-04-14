/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_fields.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_grease_pencil_point_opacity__cc {

static void node_declare(NodeDeclarationBuilder &b)
{  
  b.add_output<decl::Float>("Opacity"_ustr).field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float> opacity = AttributeFieldInput::get_field<float, "opacity">();
  params.set_output("Opacity"_ustr, std::move(opacity));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGreasePencilPointOpacity");
  ntype.ui_name = "Point Opacity";
  ntype.ui_description = "Retrieve the opacity of each point in Grease Pencil strokes";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grease_pencil_point_opacity__cc
