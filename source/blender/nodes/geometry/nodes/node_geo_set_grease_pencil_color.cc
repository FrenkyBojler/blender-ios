/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
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
  point_panel.add_input<decl::Bool>("Point").default_value(true).field_on_all().panel_toggle();
  point_panel.add_input<decl::Color>("Color", "Point Color")
      .default_value(ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f))
      .field_on_all()
      .hide_label();
  point_panel.add_input<decl::Float>("Opacity", "Point Opacity")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .field_on_all();

  PanelDeclarationBuilder &fill_panel = b.add_panel("Fill").default_closed(false).description(
      "Set fill and opacity attributes on the spline domain");
  fill_panel.add_input<decl::Bool>("Fill").default_value(true).field_on_all().panel_toggle();
  fill_panel.add_input<decl::Color>("Color", "Fill Color")
      .default_value(ColorGeometry4f(1.0f, 1.0f, 1.0f, 1.0f))
      .field_on_all()
      .hide_label();
  fill_panel.add_input<decl::Float>("Opacity", "Fill Opacity")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .field_on_all();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Grease Pencil");
  const Field<bool> point_selection = params.extract_input<Field<bool>>("Point");
  const Field<ColorGeometry4f> point_color = params.extract_input<Field<ColorGeometry4f>>(
      "Point Color");
  const Field<float> point_opacity = params.extract_input<Field<float>>("Point Opacity");

  const Field<bool> fill_selection = params.extract_input<Field<bool>>("Fill");
  const Field<ColorGeometry4f> fill_color = params.extract_input<Field<ColorGeometry4f>>(
      "Fill Color");
  const Field<float> fill_opacity = params.extract_input<Field<float>>("Fill Opacity");

  if (GreasePencil *grease_pencil = geometry_set.get_grease_pencil_for_write()) {
    using namespace blender::bke::greasepencil;
    for (const int layer_index : grease_pencil->layers().index_range()) {
      Drawing *drawing = grease_pencil->get_eval_drawing(grease_pencil->layer(layer_index));
      if (drawing == nullptr) {
        continue;
      }
      bke::CurvesGeometry &curves = drawing->strokes_for_write();

      bke::GreasePencilLayerFieldContext layer_field_context_on_point(
          *grease_pencil, AttrDomain::Point, layer_index);

      /* TODO: Avoid doing this if the selection is false. */
      if (!curves.attributes().contains("opacity")) {
        curves.attributes_for_write().add(
            "opacity",
            bke::AttrDomain::Point,
            CD_PROP_FLOAT,
            bke::AttributeInitVArray(VArray<float>::ForSingle(1.0f, curves.points_num())));
      }
      bke::try_capture_field_on_geometry(curves.attributes_for_write(),
                                         layer_field_context_on_point,
                                         "vertex_color",
                                         bke::AttrDomain::Point,
                                         point_selection,
                                         point_color);
      bke::try_capture_field_on_geometry(curves.attributes_for_write(),
                                         layer_field_context_on_point,
                                         "opacity",
                                         bke::AttrDomain::Point,
                                         point_selection,
                                         point_opacity);

      bke::GreasePencilLayerFieldContext layer_field_context_on_curves(
          *grease_pencil, AttrDomain::Curve, layer_index);

      /* TODO: Avoid doing this if the selection is false. */
      if (!curves.attributes().contains("fill_opacity")) {
        curves.attributes_for_write().add(
            "fill_opacity",
            bke::AttrDomain::Curve,
            CD_PROP_FLOAT,
            bke::AttributeInitVArray(VArray<float>::ForSingle(1.0f, curves.curves_num())));
      }
      bke::try_capture_field_on_geometry(curves.attributes_for_write(),
                                         layer_field_context_on_point,
                                         "fill_color",
                                         bke::AttrDomain::Curve,
                                         fill_selection,
                                         fill_color);
      bke::try_capture_field_on_geometry(curves.attributes_for_write(),
                                         layer_field_context_on_curves,
                                         "fill_opacity",
                                         bke::AttrDomain::Curve,
                                         fill_selection,
                                         fill_opacity);
    }
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
