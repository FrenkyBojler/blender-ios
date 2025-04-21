/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_curves.hh"

#include "GEO_fit_curves.hh"
#include "GEO_randomize.hh"

#include "UI_interface.hh"

#include "NOD_rna_define.hh"

#include "BKE_geometry_set.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_fit_curves_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).field_on_all().hide_value();

  b.add_input<decl::Float>("Threshold")
      .default_value(0.01f)
      .min(0.0f)
      .max(1.0f)
      .supports_field()
      .description(
          "Error threshold that defines how well the spline matches the input positions. Lower "
          "values result in a closer fit while larger values result in a smoother but less "
          "accurate fit");

  b.add_output<decl::Geometry>("Curves").propagate_all();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = GEO_NODE_CURVE_FIT_REFIT;
}

static bke::CurvesGeometry fit_curves(const bke::CurvesGeometry &src_curves,
                                      const Field<bool> &selection_field,
                                      const Field<float> &threshold_field,
                                      const GeometryNodeFitCurvesMode mode,
                                      const AttributeFilter &attribute_filter)
{
  const bke::CurvesFieldContext field_context{src_curves, AttrDomain::Curve};
  fn::FieldEvaluator evaluator{field_context, src_curves.curves_num()};
  evaluator.add(selection_field);
  evaluator.add(threshold_field);
  evaluator.evaluate();

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

  bke::CurvesGeometry curves = geometry::fit_curves(src_curves,
                                                    evaluator.get_evaluated_as_mask(0),
                                                    evaluator.get_evaluated<float>(1),
                                                    method,
                                                    attribute_filter);

  geometry::debug_randomize_curve_order(&curves);
  return curves;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const Field<float> threshold_field = params.extract_input<Field<float>>("Threshold");
  const GeometryNodeFitCurvesMode mode = static_cast<GeometryNodeFitCurvesMode>(
      params.node().custom1);

  const NodeAttributeFilter attribute_filter = params.get_attribute_filter("Curves");
  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (const Curves *curves_id = geometry_set.get_curves()) {
      const bke::CurvesGeometry &src_curves = curves_id->geometry.wrap();
      bke::CurvesGeometry dst_curves = fit_curves(
          src_curves, selection_field, threshold_field, mode, attribute_filter);
      Curves *dst_curves_id = bke::curves_new_nomain(std::move(dst_curves));
      bke::curves_copy_parameters(*curves_id, *dst_curves_id);
      geometry_set.replace_curves(dst_curves_id);
    }
    geometry_set.keep_only_during_modify({GeometryComponent::Type::Curve});
  });

  params.set_output("Curves", std::move(geometry_set));
}

static void node_rna(StructRNA *srna)
{
  static EnumPropertyItem mode_items[] = {
      {GEO_NODE_CURVE_FIT_REFIT, "REFIT", 0, "Refit", ""},
      {GEO_NODE_CURVE_FIT_SPLIT, "SPLIT", 0, "Split", ""},
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
