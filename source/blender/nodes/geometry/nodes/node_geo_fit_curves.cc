/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_curves.hh"

#include "GEO_fit_curves.hh"
#include "GEO_randomize.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "NOD_rna_define.hh"

#include "BKE_geometry_set.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_fit_curves_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  b.add_input<decl::Geometry>("Poly Curves", "Curves")
      /* TODO: Should also support Grease Pencil. */
      .supported_type(GeometryComponent::Type::Curve);
  b.add_output<decl::Geometry>("Curves").propagate_all().align_with_previous();

  b.add_input<decl::Bool>("Selection").default_value(true).field_on_all().hide_value();

  b.add_input<decl::Bool>("Corners").default_value(false).field_on_all().hide_value();

  b.add_input<decl::Float>("Error")
      .default_value(0.01f)
      .min(0.0f)
      .subtype(PROP_DISTANCE)
      .supports_field()
      .description("The error distance that the resulting points are allowed to be within");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = GEO_NODE_CURVE_FIT_SPLIT;
}

static bke::CurvesGeometry fit_curves(const bke::CurvesGeometry &src_curves,
                                      const IndexMask &poly_curves,
                                      const Field<bool> &selection_field,
                                      const Field<bool> &corners_field,
                                      const Field<float> &threshold_field,
                                      const GeometryNodeFitCurvesMode mode,
                                      const AttributeFilter &attribute_filter)
{
  const bke::CurvesFieldContext curve_field_context{src_curves, AttrDomain::Curve};
  fn::FieldEvaluator curve_evaluator{curve_field_context, &poly_curves};
  curve_evaluator.set_selection(selection_field);
  curve_evaluator.add(threshold_field);
  curve_evaluator.evaluate();

  const bke::CurvesFieldContext point_field_context{src_curves, AttrDomain::Point};
  fn::FieldEvaluator point_evaluator{point_field_context, src_curves.points_num()};
  point_evaluator.add(corners_field);
  point_evaluator.evaluate();

  geometry::FitMethod method;
  switch (mode) {
    case GEO_NODE_CURVE_FIT_SPLIT:
      method = geometry::FitMethod::Split;
      break;
    case GEO_NODE_CURVE_FIT_REFIT:
      method = geometry::FitMethod::Refit;
      break;
    default:
      BLI_assert_unreachable();
  }

  bke::CurvesGeometry curves = geometry::fit_poly_to_bezier_curves(
      src_curves,
      curve_evaluator.get_evaluated_selection_as_mask(),
      curve_evaluator.get_evaluated<float>(0),
      point_evaluator.get_evaluated<bool>(0),
      method,
      attribute_filter);

  geometry::debug_randomize_curve_order(&curves);
  return curves;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const Field<bool> corners_field = params.extract_input<Field<bool>>("Corners");
  const Field<float> threshold_field = params.extract_input<Field<float>>("Error");
  const GeometryNodeFitCurvesMode mode = static_cast<GeometryNodeFitCurvesMode>(
      params.node().custom1);

  const NodeAttributeFilter attribute_filter = params.get_attribute_filter("Curves");
  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry_set) {
    if (const Curves *curves_id = geometry_set.get_curves()) {
      const bke::CurvesGeometry &src_curves = curves_id->geometry.wrap();
      if (!src_curves.has_curve_with_type(CURVE_TYPE_POLY)) {
        params.error_message_add(NodeWarningType::Warning, "Input curves have no poly curves");
        return;
      }
      IndexMaskMemory memory;
      const IndexMask poly_curves = src_curves.indices_for_curve_type(CURVE_TYPE_POLY, memory);
      bke::CurvesGeometry dst_curves = fit_curves(src_curves,
                                                  poly_curves,
                                                  selection_field,
                                                  corners_field,
                                                  threshold_field,
                                                  mode,
                                                  attribute_filter);
      Curves *dst_curves_id = bke::curves_new_nomain(std::move(dst_curves));
      bke::curves_copy_parameters(*curves_id, *dst_curves_id);
      geometry_set.replace_curves(dst_curves_id);
    }
  });

  params.set_output("Curves", std::move(geometry_set));
}

static void node_rna(StructRNA *srna)
{
  static EnumPropertyItem mode_items[] = {
      {GEO_NODE_CURVE_FIT_SPLIT,
       "SPLIT",
       0,
       "Split",
       "Uses a least squares solver to find the control points (faster, but less accurate)"},
      {GEO_NODE_CURVE_FIT_REFIT,
       "REFIT",
       0,
       "Refit",
       "Iteratively removes knots with the least error starting with a dense curve (slower, more "
       "accurate fit)"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(
      srna, "mode", "Mode", "Curve fitting mode", mode_items, NOD_inline_enum_accessors(custom1));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeFitCurves");
  ntype.ui_name = "Fit Curves";
  ntype.ui_description = "Fit the points of the input curves to bézier curves";
  ntype.declare = node_declare;
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.initfunc = node_init;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_fit_curves_cc
