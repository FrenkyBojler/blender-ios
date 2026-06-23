/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_delaunay_2d.hh"
#include "BLI_math_vector_types.hh"

#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_instances.hh"
#include "BKE_mesh.hh"

#include "GEO_CDT_to_mesh.hh"

#include "BLI_task.hh"

#include "GEO_foreach_geometry.hh"
#include "GEO_join_geometries.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_curve_fill_cc {

NODE_STORAGE_FUNCS(NodeGeometryCurveFill)

static const EnumPropertyItem mode_items[] = {
    {GEO_NODE_CURVE_FILL_MODE_TRIANGULATED, "TRIANGLES", 0, N_("Triangles"), ""},
    {GEO_NODE_CURVE_FILL_MODE_NGONS, "NGONS", 0, N_("N-gons"), ""},
    {0, nullptr, 0, nullptr, nullptr},
};

/* See #CDT_output_type in BLI_delaunay_2d.hh for winding rule details. */
static const EnumPropertyItem fill_rule_items[] = {
    {GEO_NODE_CURVE_FILL_RULE_EVEN_ODD,
     "EVEN_ODD",
     0,
     N_("Even-Odd"),
     N_("Alternate inside/outside based on crossing count")},
    {GEO_NODE_CURVE_FILL_RULE_NON_ZERO,
     "NON_ZERO",
     0,
     N_("Non-Zero"),
     N_("Overlapping curves with the same winding direction are filled as a union")},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Curve"_ustr)
      .supported_type({GeometryComponent::Type::Curve, GeometryComponent::Type::GreasePencil})
      .description(
          "Curves to fill. All curves are treated as cyclic and projected to the XY plane");
  b.add_input<decl::Int>("Group ID"_ustr)
      .evaluated_geometry_field()
      .hide_value()
      .description(
          "An index used to group curves together. Filling is done separately for each group");
  b.add_input<decl::Menu>("Mode"_ustr)
      .static_items(mode_items)
      .default_value(GEO_NODE_CURVE_FILL_MODE_TRIANGULATED)
      .optional_label();
  b.add_input<decl::Menu>("Fill Rule"_ustr)
      .static_items(fill_rule_items)
      .default_value(GEO_NODE_CURVE_FILL_RULE_EVEN_ODD)
      .optional_label()
      .description("Rule used to determine which regions are inside or outside");
  b.add_output<decl::Geometry>("Mesh"_ustr).propagate_all_geometry();
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  /* Still used for forward compatibility. */
  node->storage = MEM_new<NodeGeometryCurveFill>(__func__);
}

static void fill_curve_vert_indices(const OffsetIndices<int> offsets,
                                    MutableSpan<Vector<int>> faces)
{
  threading::parallel_for(faces.index_range(), 1024, [&](const IndexRange range) {
    for (const int i : range) {
      faces[i].resize(offsets[i].size());
      array_utils::fill_index_range<int>(faces[i], offsets[i].start());
    }
  });
}

static meshintersect::CDT_result<double> do_cdt(const bke::CurvesGeometry &curves,
                                                const CDT_output_type output_type)
{
  const OffsetIndices points_by_curve = curves.evaluated_points_by_curve();
  const Span<float3> positions = curves.evaluated_positions();

  Array<double2> positions_2d(positions.size());
  threading::parallel_for(positions.index_range(), 2048, [&](const IndexRange range) {
    for (const int i : range) {
      positions_2d[i] = double2(positions[i].x, positions[i].y);
    }
  });

  Array<Vector<int>> faces(curves.curves_num());
  fill_curve_vert_indices(points_by_curve, faces);

  meshintersect::CDT_input<double> input;
  input.need_ids = false;
  input.vert = std::move(positions_2d);
  input.face = std::move(faces);

  return delaunay_2d_calc(input, output_type);
}

static meshintersect::CDT_result<double> do_cdt_with_mask(const bke::CurvesGeometry &curves,
                                                          const CDT_output_type output_type,
                                                          const IndexMask &mask)
{
  const OffsetIndices points_by_curve = curves.evaluated_points_by_curve();
  const Span<float3> positions = curves.evaluated_positions();

  Array<int> offsets_data(mask.size() + 1);
  const OffsetIndices points_by_curve_masked = offset_indices::gather_selected_offsets(
      points_by_curve, mask, offsets_data);

  Array<double2> positions_2d(points_by_curve_masked.total_size());
  mask.foreach_index(
      [&](const int src_curve, const int dst_curve) {
        const IndexRange src_points = points_by_curve[src_curve];
        const IndexRange dst_points = points_by_curve_masked[dst_curve];
        for (const int i : src_points.index_range()) {
          const int src = src_points[i];
          const int dst = dst_points[i];
          positions_2d[dst] = double2(positions[src].x, positions[src].y);
        }
      },
      exec_mode::grain_size(1024));

  Array<Vector<int>> faces(points_by_curve_masked.size());
  fill_curve_vert_indices(points_by_curve_masked, faces);

  meshintersect::CDT_input<double> input;
  input.need_ids = false;
  input.vert = std::move(positions_2d);
  input.face = std::move(faces);

  return delaunay_2d_calc(input, output_type);
}

static Array<meshintersect::CDT_result<double>> do_group_aware_cdt(
    const bke::CurvesGeometry &curves,
    const CDT_output_type output_type,
    const Field<int> &group_index_field)
{
  const bke::GeometryFieldContext field_context{curves, AttrDomain::Curve};
  fn::FieldEvaluator data_evaluator{field_context, curves.curves_num()};
  data_evaluator.add(group_index_field);
  data_evaluator.evaluate();
  const VArray<int> curve_group_ids = data_evaluator.get_evaluated<int>(0);

  if (curve_group_ids.is_single()) {
    return {do_cdt(curves, output_type)};
  }

  VectorSet<int> group_indexing;
  IndexMaskMemory mask_memory;
  const Vector<IndexMask> group_masks = IndexMask::from_group_ids(
      curve_group_ids, mask_memory, group_indexing);
  const int groups_num = group_masks.size();

  Array<meshintersect::CDT_result<double>> cdt_results(groups_num);

  /* The grain size should be larger as each group gets smaller. */
  const int domain_size = curve_group_ids.size();
  const int avg_group_size = domain_size / groups_num;
  const int grain_size = std::max(8192 / avg_group_size, 1);
  threading::parallel_for(IndexRange(groups_num), grain_size, [&](const IndexRange range) {
    for (const int group_index : range) {
      const IndexMask &mask = group_masks[group_index];
      cdt_results[group_index] = do_cdt_with_mask(curves, output_type, mask);
    }
  });

  return cdt_results;
}

static void curve_fill_calculate(GeometrySet &geometry_set,
                                 const GeometryNodeCurveFillMode mode,
                                 const GeometryNodeCurveFillRule fill_rule,
                                 const Field<int> &group_index)
{
  /* Determine CDT output type based on mode and fill rule. */
  CDT_output_type output_type;
  if (mode == GEO_NODE_CURVE_FILL_MODE_NGONS) {
    output_type = CDT_CONSTRAINTS_VALID_BMESH_WITH_HOLES;
    switch (fill_rule) {
      case GEO_NODE_CURVE_FILL_RULE_NON_ZERO: {
        output_type = CDT_CONSTRAINTS_VALID_BMESH_WITH_HOLES_NONZERO;
        break;
      }
      case GEO_NODE_CURVE_FILL_RULE_EVEN_ODD: {
        /* Default, already set. */
        break;
      }
    }
  }
  else {
    /* Triangulated mode. */
    output_type = CDT_INSIDE_WITH_HOLES;
    switch (fill_rule) {
      case GEO_NODE_CURVE_FILL_RULE_NON_ZERO: {
        output_type = CDT_INSIDE_WITH_HOLES_NONZERO;
        break;
      }
      case GEO_NODE_CURVE_FILL_RULE_EVEN_ODD: {
        /* Default, already set. */
        break;
      }
    }
  }
  if (geometry_set.has_curves()) {
    const Curves &curves_id = *geometry_set.get_curves();
    const bke::CurvesGeometry &curves = curves_id.geometry.wrap();
    if (curves.curves_num() > 0) {
      const Array<meshintersect::CDT_result<double>> results = do_group_aware_cdt(
          curves, output_type, group_index);
      Mesh *mesh = geometry::cdts_to_mesh(results);
      geometry_set.replace_mesh(mesh);
    }
    geometry_set.replace_curves(nullptr);
  }

  if (geometry_set.has_grease_pencil()) {
    using namespace blender::bke::greasepencil;
    const GreasePencil &grease_pencil = *geometry_set.get_grease_pencil();
    Vector<Mesh *> mesh_by_layer(grease_pencil.layers().size(), nullptr);
    for (const int layer_index : grease_pencil.layers().index_range()) {
      const Drawing *drawing = grease_pencil.get_eval_drawing(grease_pencil.layer(layer_index));
      if (drawing == nullptr) {
        continue;
      }
      const bke::CurvesGeometry &src_curves = drawing->strokes();
      if (src_curves.is_empty()) {
        continue;
      }
      const Array<meshintersect::CDT_result<double>> results = do_group_aware_cdt(
          src_curves, output_type, group_index);
      mesh_by_layer[layer_index] = geometry::cdts_to_mesh(results);
    }
    if (!mesh_by_layer.is_empty()) {
      auto instances = std::make_unique<bke::Instances>(mesh_by_layer.size());
      MutableSpan<int> handles = instances->reference_handles_for_write();
      instances->transforms_for_write().fill(float4x4::identity());
      for (const int i : mesh_by_layer.index_range()) {
        Mesh *mesh = mesh_by_layer[i];
        if (!mesh) {
          /* Add an empty reference so the number of layers and instances match.
           * This makes it easy to reconstruct the layers afterwards and keep their attributes.
           * Although in this particular case we don't propagate the attributes. */
          handles[i] = instances->add_reference(bke::InstanceReference());
          continue;
        }
        GeometrySet temp_set = GeometrySet::from_mesh(mesh);
        handles[i] = instances->add_reference(bke::InstanceReference{temp_set});
      }
      auto &dst_component = geometry_set.get_component_for_write<InstancesComponent>();
      GeometrySet new_instances = geometry::join_geometries(
          {GeometrySet::from_instances(dst_component.release()),
           GeometrySet::from_instances(std::move(instances))},
          {});
      dst_component.replace(new_instances.get_component_for_write<InstancesComponent>().release());
    }
    geometry_set.replace_grease_pencil(nullptr);
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Curve"_ustr);
  Field<int> group_index = params.extract_input<Field<int>>("Group ID"_ustr);
  const GeometryNodeCurveFillMode mode = params.extract_input<GeometryNodeCurveFillMode>(
      "Mode"_ustr);
  const auto fill_rule = params.extract_input<GeometryNodeCurveFillRule>("Fill Rule"_ustr);

  geometry::foreach_real_geometry(geometry_set, [&](GeometrySet &geometry) {
    curve_fill_calculate(geometry, mode, fill_rule, group_index);
  });

  params.set_output("Mesh"_ustr, std::move(geometry_set));
}

static void node_register()
{
  static bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeFillCurve"_ustr, GEO_NODE_FILL_CURVE);
  ntype.ui_name = "Fill Curve";
  ntype.ui_description =
      "Generate a mesh on the XY plane with faces on the inside of input curves";
  ntype.enum_name_legacy = "FILL_CURVE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.initfunc = node_init;
  bke::node_type_storage(
      ntype, "NodeGeometryCurveFill", node_free_standard_storage, node_copy_standard_storage);
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_curve_fill_cc
