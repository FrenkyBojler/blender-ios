/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"

#include "FN_multi_function_builder.hh"

#include "GEO_foreach_geometry.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_set_spline_order_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_input<decl::Geometry>("Curve", "Geometry")
      .supported_type({GeometryComponent::Type::Curve, GeometryComponent::Type::GreasePencil})
      .description("Curves to change the order of");
  b.add_output<decl::Geometry>("Curve", "Geometry").propagate_all().align_with_previous();
  b.add_input<decl::Bool>("Selection").default_value(true).hide_value().field_on_all();
  b.add_input<decl::Int>("Order").default_value(4).min(2).max(127).supports_field();
}

static fn::Field<int8_t> clamp_to_int8(fn::Field<int> order_field)
{
  static auto clamp_fn = mf::build::SI1_SO<int, int8_t>(
      "clamp_to_int8",
      [](int value) { return int8_t(std::clamp(value, 2, 127)); },
      mf::build::exec_presets::AllSpanOrSingle());
  return fn::Field<int8_t>(fn::FieldOperation::from(clamp_fn, {std::move(order_field)}));
}

static void set_curve_order(bke::CurvesGeometry &curves,
                            const fn::FieldContext &field_context,
                            const Field<bool> &selection,
                            const Field<int8_t> &order)
{
  bke::try_capture_field_on_geometry(curves.attributes_for_write(),
                                     field_context,
                                     "nurbs_order",
                                     bke::AttrDomain::Curve,
                                     selection,
                                     order);
}

static void set_grease_pencil_order(GreasePencil &grease_pencil,
                                    const Field<bool> &selection,
                                    const Field<int8_t> &order,
                                    std::atomic<bool> &has_nurbs)
{
  using namespace blender::bke::greasepencil;
  for (const int layer_index : grease_pencil.layers().index_range()) {
    Drawing *drawing = grease_pencil.get_eval_drawing(grease_pencil.layer(layer_index));
    if (drawing == nullptr) {
      continue;
    }
    bke::CurvesGeometry &curves = drawing->strokes_for_write();
    has_nurbs = curves.has_curve_with_type(CURVE_TYPE_NURBS);
    if(!has_nurbs) {
      continue;
    }
    set_curve_order(
        curves,
        bke::GreasePencilLayerFieldContext(grease_pencil, AttrDomain::Curve, layer_index),
        selection,
        order);
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  const Field<bool> selection = params.extract_input<Field<bool>>("Selection");
  const Field<int> order = params.extract_input<Field<int>>("Order");
  const Field<int8_t> order_int8 = clamp_to_int8(order);
  std::atomic<bool> has_nurbs = false;

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (Curves *curves_id = geometry_set.get_curves_for_write()) {
      bke::CurvesGeometry &curves = curves_id->geometry.wrap();
      has_nurbs = curves.has_curve_with_type(CURVE_TYPE_NURBS);
      if (has_nurbs) {
        const bke::CurvesFieldContext field_context{*curves_id, AttrDomain::Curve};
        set_curve_order(curves, field_context, selection, order_int8);
      }
    }
    if (GreasePencil *grease_pencil = geometry_set.get_grease_pencil_for_write()) {
      set_grease_pencil_order(*grease_pencil, selection, order_int8, has_nurbs);
    }
  });

  if (!has_nurbs) {
    params.error_message_add(NodeWarningType::Info, TIP_("Input curves do not have NURBS type"));
  }

  params.set_output("Geometry", std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeSetSplineOrder", GEO_NODE_SET_SPLINE_ORDER);
  ntype.ui_name = "Set Spline Order";
  ntype.ui_description = "Control the order of each spline by changing the \"order\" attribute";
  ntype.enum_name_legacy = "SET_SPLINE_ORDER";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_set_spline_order_cc
