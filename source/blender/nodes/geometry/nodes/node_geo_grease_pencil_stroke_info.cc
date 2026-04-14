/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_geometry_fields.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_grease_pencil_stroke_info__cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_output<decl::Float>("Softness"_ustr).field_source();
  b.add_output<decl::Bool>("Is Hidden"_ustr).field_source();
  b.add_output<decl::Float>("Creation Time"_ustr).field_source();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<float> softness = AttributeFieldInput::get_field<float, "softness">();
  params.set_output("Softness"_ustr, std::move(softness));

  Field<bool> hide_stroke = AttributeFieldInput::get_field<bool, "hide_stroke">();
  params.set_output("Is Hidden"_ustr, std::move(hide_stroke));

  Field<float> init_time = AttributeFieldInput::get_field<float, "init_time">();
  params.set_output("Creation Time"_ustr, std::move(init_time));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeGreasePencilStrokeInfo");
  ntype.ui_name = "Stroke Info";
  ntype.ui_description = "Retrieve information about Grease Pencil strokes";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_grease_pencil_stroke_info__cc
