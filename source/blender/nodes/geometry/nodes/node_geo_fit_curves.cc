/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_curves.hh"

#include "GEO_fit_curves.hh"

#include "NOD_rna_define.hh"

#include "BKE_geometry_set.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_fit_curves_cc {

NODE_STORAGE_FUNCS(NodeGeometryFitCurves)

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
  NodeGeometryFitCurves *data = MEM_cnew<NodeGeometryFitCurves>(__func__);

  data->mode = GEO_NODE_CURVE_FIT_REFIT;
  node->storage = data;
}

static bke::CurvesGeometry fit_curves(const bke::CurvesGeometry &src_curves,
                                      const Field<bool> &selection_field,
                                      const Field<float> &threshold_field,
                                      const AttributeFilter &attribute_filter)
{
  const bke::CurvesFieldContext field_context{src_curves, AttrDomain::Curve};
  fn::FieldEvaluator evaluator{field_context, src_curves.curves_num()};
  evaluator.add(selection_field);
  evaluator.add(threshold_field);
  evaluator.evaluate();

  Array<int> old_to_new_map;
  bke::CurvesGeometry curves = geometry::fit_curves(src_curves.positions(),
                                                    src_curves.points_by_curve(),
                                                    evaluator.get_evaluated_as_mask(0),
                                                    src_curves.cyclic(),
                                                    evaluator.get_evaluated<float>(1),
                                                    geometry::FitMethod::Refit,
                                                    old_to_new_map);

  bke::gather_attributes(src_curves.attributes(),
                         AttrDomain::Point,
                         AttrDomain::Point,
                         attribute_filter,
                         old_to_new_map,
                         curves.attributes_for_write());

  geometry::debug_randomize_curve_order(&curves);
  return curves;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const Field<float> threshold_field = params.extract_input<Field<float>>("Threshold");

  const NodeAttributeFilter attribute_filter = params.get_attribute_filter("Curves");
  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (const Curves *curves_id = geometry_set.get_curves()) {
      const bke::CurvesGeometry &src_curves = curves_id->geometry.wrap();
      bke::CurvesGeometry dst_curves = fit_curves(
          src_curves, selection_field, threshold_field, attribute_filter);
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
      {GEO_NODE_CURVE_FIT_SPLIT, "SPLIT", 0, "Split", ""},
      {GEO_NODE_CURVE_FIT_REFIT, "REFIT", 0, "Refit", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  RNA_def_node_enum(
      srna, "mode", "Mode", "Curve fitting mode", mode_items, NOD_storage_enum_accessors(mode));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_FIT_CURVES, "Fit Curves", NODE_CLASS_GEOMETRY);
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  blender::bke::node_type_storage(
      &ntype, "NodeGeometryFitCurves", node_free_standard_storage, node_copy_standard_storage);
  ntype.initfunc = node_init;
  ntype.geometry_node_execute = node_geo_exec;
  blender::bke::node_register_type(&ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_fit_curves_cc
