/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_fields.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_grease_pencil_fill_info__cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Color>("Color"_ustr).field_source();
  b.add_output<decl::Float>("Opacity"_ustr).field_source();
  b.add_output<decl::Int>("Fill ID"_ustr).field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<ColorGeometry4f> fill_color = AttributeFieldInput::get_field<ColorGeometry4f, "fill_color">();
  params.set_output("Color"_ustr, std::move(fill_color));

  Field<float> opacity = AttributeFieldInput::get_field<float, "fill_opacity">();
  params.set_output("Opacity"_ustr, std::move(opacity));

  Field<int> fill_id = AttributeFieldInput::get_field<int, "fill_id">();
  params.set_output("Fill ID"_ustr, std::move(fill_id));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGreasePencilFillInfo");
  ntype.ui_name = "Fill Info";
  ntype.ui_description = "Retrieve information about Grease Pencil fills";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grease_pencil_fill_info__cc
