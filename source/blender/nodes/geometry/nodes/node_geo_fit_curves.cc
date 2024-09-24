/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"

#include "BLI_array_utils.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task.hh"

#include "DNA_pointcloud_types.h"

#include "GEO_fit_curves.hh"

#include "BKE_geometry_set.hh"

namespace blender::nodes::node_geo_fit_curves_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curves").supported_type(GeometryComponent::Type::Curve);
  b.add_input<decl::Bool>("Selection").default_value(true).field_on_all().hide_value();

  b.add_input<decl::Float>("Threshold")
      .default_value(0.01f)
      .min(0.0f)
      .max(1000.0f)
      .supports_field()
      .description(
          "Error threshold that defines how well the spline matches the input positions. Lower "
          "values result in a closer fit while larger values result in a smoother but less "
          "accurate fit");

  b.add_output<decl::Geometry>("Curves").propagate_all();
}

static Curves *fit_curves(const Curves &curves,
                          const Field<bool> &selection_field,
                          const Field<float> &threshold_field,
                          const AttributeFilter &attribute_filter)
{
  // const int domain_size = points.totpoint;
  // if (domain_size == 0) {
  //   return nullptr;
  // }

  // Array<int> group_ids(domain_size);
  // {
  //   const bke::PointCloudFieldContext context(points);
  //   fn::FieldEvaluator evaluator(context, domain_size);
  //   evaluator.add(group_id_field);
  //   evaluator.evaluate();

  //   const VArray<int> group_ids_varray = evaluator.get_evaluated<int>(0);
  //   group_ids_varray.materialize(group_ids.as_mutable_span());
  // }
  // const int total_curves = identifiers_to_indices(group_ids);
  Curves *curves_id = bke::curves_new_nomain(0, 0);
  // bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  // Array<int> old_to_new_map;
  // {
  //   const bke::CurvesFieldContext context(curves, bke::AttrDomain::Curve);
  //   fn::FieldEvaluator evaluator(context, total_curves);
  //   evaluator.add(cyclic_field);
  //   evaluator.add(resolution_field);
  //   evaluator.evaluate();

  //   const VArray<bool> cyclic_varray = evaluator.get_evaluated<bool>(0);
  //   const VArray<int> resolution_varray = evaluator.get_evaluated<int>(1);

  //   const Span<int> indices = group_ids.as_span();
  //   curves.offsets_for_write().fill(0);
  //   offset_indices::build_reverse_offsets(indices, curves.offsets_for_write());

  //   const OffsetIndices src_point_offsets = curves.offsets();
  //   curves = geometry::fit_curves(points.positions(),
  //                                 src_point_offsets,
  //                                 cyclic_varray,
  //                                 resolution_varray,
  //                                 epsilon,
  //                                 old_to_new_map);
  // }

  // bke::gather_attributes(points.attributes(),
  //                        AttrDomain::Point,
  //                        AttrDomain::Point,
  //                        attribute_filter,
  //                        old_to_new_map,
  //                        curves.attributes_for_write());

  // geometry::debug_randomize_curve_order(&curves);
  return curves_id;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curves");
  const Field<bool> selection_field = params.extract_input<Field<bool>>("Selection");
  const Field<float> threshold_field = params.extract_input<Field<float>>("Threshold");

  const NodeAttributeFilter attribute_filter = params.get_attribute_filter("Curves");
  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (const Curves *input_curves = geometry_set.get_curves()) {
      Curves *result_curves = fit_curves(
          *input_curves, selection_field, threshold_field, attribute_filter);
      geometry_set.replace_curves(result_curves);
    }
    geometry_set.keep_only_during_modify({GeometryComponent::Type::Curve});
  });

  params.set_output("Curves", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, GEO_NODE_FIT_CURVES, "Fit Curves", NODE_CLASS_GEOMETRY);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  blender::bke::node_register_type(&ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_fit_curves_cc
