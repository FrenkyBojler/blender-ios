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

#include "GEO_fit_curves_to_points.hh"

#include "BKE_geometry_set.hh"

namespace blender::nodes::node_geo_fit_curves_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Points")
      .supported_type({GeometryComponent::Type::PointCloud, GeometryComponent::Type::Curve})
      .description("Points to fit curves to");

  b.add_input<decl::Int>("Curve Group ID")
      .field_on_all()
      .hide_value()
      .description(
          "A curve is created for every distinct group ID. All points with the same ID will be "
          "used to fit the same curve");

  b.add_input<decl::Bool>("Cyclic").supports_field().hide_value().description(
      "Determines which curves should be cyclic");
  b.add_input<decl::Int>("Resolution")
      .default_value(16)
      .supports_field()
      .description("Determines the resolution of each curve");

  b.add_input<decl::Float>("Epsilon").default_value(0.01f);

  b.add_output<decl::Geometry>("Curves").propagate_all();
}

static int identifiers_to_indices(MutableSpan<int> r_identifiers_to_indices)
{
  const VectorSet<int> deduplicated_groups(r_identifiers_to_indices);
  threading::parallel_for(
      r_identifiers_to_indices.index_range(), 2048, [&](const IndexRange range) {
        for (int &value : r_identifiers_to_indices.slice(range)) {
          value = deduplicated_groups.index_of(value);
        }
      });
  return deduplicated_groups.size();
}

static Curves *fit_curves_from_points(const PointCloud &points,
                                      const Field<int> &group_id_field,
                                      const Field<bool> &cyclic_field,
                                      const Field<int> &resolution_field,
                                      const float epsilon,
                                      const AttributeFilter &attribute_filter)
{
  const int domain_size = points.totpoint;
  if (domain_size == 0) {
    return nullptr;
  }

  Array<int> group_ids(domain_size);
  {
    const bke::PointCloudFieldContext context(points);
    fn::FieldEvaluator evaluator(context, domain_size);
    evaluator.add(group_id_field);
    evaluator.evaluate();

    const VArray<int> group_ids_varray = evaluator.get_evaluated<int>(0);
    group_ids_varray.materialize(group_ids.as_mutable_span());
  }
  const int total_curves = identifiers_to_indices(group_ids);
  Curves *curves_id = bke::curves_new_nomain(domain_size, total_curves);
  bke::CurvesGeometry &curves = curves_id->geometry.wrap();
  Array<int> old_to_new_map;
  {
    const bke::CurvesFieldContext context(curves, bke::AttrDomain::Curve);
    fn::FieldEvaluator evaluator(context, total_curves);
    evaluator.add(cyclic_field);
    evaluator.add(resolution_field);
    evaluator.evaluate();

    const VArray<bool> cyclic_varray = evaluator.get_evaluated<bool>(0);
    const VArray<int> resolution_varray = evaluator.get_evaluated<int>(1);

    const Span<int> indices = group_ids.as_span();
    curves.offsets_for_write().fill(0);
    offset_indices::build_reverse_offsets(indices, curves.offsets_for_write());

    const OffsetIndices src_point_offsets = curves.offsets();
    curves = geometry::fit_curves_to_points(points.positions(),
                                            src_point_offsets,
                                            cyclic_varray,
                                            resolution_varray,
                                            epsilon,
                                            old_to_new_map);
  }

  bke::gather_attributes(points.attributes(),
                         AttrDomain::Point,
                         AttrDomain::Point,
                         attribute_filter,
                         old_to_new_map,
                         curves.attributes_for_write());

  geometry::debug_randomize_curve_order(&curves);
  return curves_id;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Points");
  const Field<int> group_id_field = params.extract_input<Field<int>>("Curve Group ID");
  const Field<bool> cyclic_field = params.extract_input<Field<bool>>("Cyclic");
  const Field<int> resolution_field = params.extract_input<Field<int>>("Resolution");
  const float epsilon = params.extract_input<float>("Epsilon");

  const NodeAttributeFilter attribute_filter = params.get_attribute_filter("Curves");
  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    if (const PointCloud *points = geometry_set.get_pointcloud()) {
      Curves *curves_id = fit_curves_from_points(
          *points, group_id_field, cyclic_field, resolution_field, epsilon, attribute_filter);
      geometry_set.replace_curves(curves_id);
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
