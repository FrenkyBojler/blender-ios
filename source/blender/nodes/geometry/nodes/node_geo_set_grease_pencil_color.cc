/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_grease_pencil.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_grease_pencil_color_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  b.add_output<decl::Geometry>("Grease Pencil").propagate_all();
  b.add_input<decl::Geometry>("Grease Pencil")
      .supported_type(GeometryComponent::Type::GreasePencil)
      .align_with_previous();

  PanelDeclarationBuilder &point_panel = b.add_panel("Point").default_closed(false).description(
      "Set color and opacity attributes on the point domain");
  point_panel.add_input<decl::Bool>("Point").default_value(true).panel_toggle();
  point_panel.add_input<decl::Color>("Color")
      .default_value(ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f))
      .field_on_all()
      .hide_label();
  point_panel.add_input<decl::Float>("Opacity").default_value(1.0f).field_on_all();

  PanelDeclarationBuilder &fill_panel = b.add_panel("Fill").default_closed(false).description(
      "Set fill and opacity attributes on the spline domain");
  fill_panel.add_input<decl::Bool>("Fill").default_value(true).panel_toggle();
  fill_panel.add_input<decl::Color>("Color")
      .default_value(ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f))
      .field_on_all()
      .hide_label();
  fill_panel.add_input<decl::Float>("Opacity").default_value(1.0f).field_on_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Grease Pencil");

  if (GreasePencil *grease_pencil = geometry_set.get_grease_pencil_for_write()) {
    /* TODO */
  }

  params.set_output("Grease Pencil", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSetGreasePencilColor");
  ntype.ui_name = "Set Grease Pencil Color";
  ntype.ui_description = "Set color and opacity attributes on Grease Pencil geometry";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_type_size(ntype, 170, 120, NODE_DEFAULT_MAX_WIDTH);
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_grease_pencil_color_cc
