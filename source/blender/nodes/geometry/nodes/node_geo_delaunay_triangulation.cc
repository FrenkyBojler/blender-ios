/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_delaunay_2d.hh"
#include "BLI_index_mask.hh"

#include "BKE_curves.hh"
#include "BKE_mesh.hh"
#include "BKE_pointcloud.hh"

#include "FN_field.hh"

#include "GEO_CDT_to_mesh.hh"
#include "GEO_mesh_copy_selection.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_delaunay_triangulation_cc {

enum class TriangulationMode : int8_t {
  Full = 0,
  Inside = 1,
  InsideWidthHoles = 2,
};

static const EnumPropertyItem mode_items[] = {
    {int(TriangulationMode::Full),
     "FULL",
     0,
     "Full",
     "All triangles. The outer boundary is the convex hull of input points"},
    {int(TriangulationMode::Inside),
     "INSIDE",
     0,
     "Inside",
     "All triangles fully enclosed by constraint edges or faces"},
    {int(TriangulationMode::InsideWidthHoles),
     "INSIDE_WITH_HOLES",
     0,
     "Inside With Holes",
     "Triangles fully enclosed by constraint edges or faces excluding triangles inside detected "
     "holes"},
    {0, nullptr, 0, nullptr, nullptr},
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry")
      .supported_type({GeometryComponent::Type::Mesh,
                       GeometryComponent::Type::Curve,
                       //  GeometryComponent::Type::GreasePencil, /* TODO */
                       GeometryComponent::Type::PointCloud})
      .description(
          "The geometries that are used to constrain the triangulation using the points, edges, "
          "and faces");
  b.add_input<decl::Int>("Group ID")
      .field_on_all()
      .hide_value()
      .description(
          "An index used to group points together. Triangulation is done separately for each "
          "group");
  b.add_input<decl::Menu>("Mode")
      .static_items(mode_items)
      .default_value(int(TriangulationMode::Full));
  b.add_output<decl::Geometry>("Mesh").propagate_all();
  b.add_output<decl::Int>("Group ID")
      .field_on_all()
      .description("The group ID of each triangulation group");
}

static CDT_output_type get_cdt_output_type(const TriangulationMode mode)
{
  switch (mode) {
    case TriangulationMode::Full:
      return CDT_FULL;
    case TriangulationMode::Inside:
      return CDT_INSIDE;
    case TriangulationMode::InsideWidthHoles:
      return CDT_INSIDE_WITH_HOLES;
  }
  return CDT_FULL;
}

struct CDTGeometrySetInput {
  GeometrySet geometry;
  meshintersect::CDT_input<double> cdt_input;
  Vector<int> point_components;
  Vector<int> edge_components;
  Vector<int> face_components;

  Vector<int> point_offsets;
  Vector<int> edge_offsets;
  Vector<int> face_offsets;

  OffsetIndices<int> points_by_components() const
  {
    return OffsetIndices<int>(point_offsets.as_span());
  }
  OffsetIndices<int> edges_by_components() const
  {
    return OffsetIndices<int>(edge_offsets.as_span());
  }
  OffsetIndices<int> faces_by_components() const
  {
    return OffsetIndices<int>(face_offsets.as_span());
  }
};

static Array<meshintersect::CDT_result<double>> calculate_cdts(
    const Span<CDTGeometrySetInput> inputs, const CDT_output_type output_type)
{
  Array<meshintersect::CDT_result<double>> outputs(inputs.size());
  /* TODO: Use better grain size. */
  threading::parallel_for(inputs.index_range(), 8, [&](const IndexRange range) {
    for (const int i : range) {
      outputs[i] = meshintersect::delaunay_2d_calc(inputs[i].cdt_input, output_type);
    }
  });

  return outputs;
}

static void copy_positions_float3_to_double2(const Span<float3> src_positions,
                                             MutableSpan<double2> dst_positions)
{
  threading::parallel_for(src_positions.index_range(), 8192, [&](const IndexRange range) {
    for (const int i : range) {
      dst_positions[i] = double2(src_positions[i].x, src_positions[i].y);
    }
  });
}

static std::optional<CDTGeometrySetInput> cdt_input_from_geometry_set(
    const GeometrySet &geometry_set)
{
  if (geometry_set.is_empty()) {
    return std::nullopt;
  }
  CDTGeometrySetInput input;
  input.geometry = geometry_set;
  Vector<const GeometryComponent *> all_components = input.geometry.get_components();
  for (const int component_i : all_components.index_range()) {
    const GeometryComponent *component = all_components[component_i];
    switch (component->type()) {
      case GeometryComponent::Type::PointCloud: {
        const PointCloud &pointcloud = *static_cast<const PointCloudComponent *>(component)->get();
        if (pointcloud.totpoint > 0) {
          input.point_components.append(component_i);
          input.point_offsets.append(pointcloud.totpoint);
        }
        break;
      }
      case GeometryComponent::Type::Curve: {
        const Curves &curves_component = *static_cast<const CurveComponent *>(component)->get();
        const bke::CurvesGeometry &curves = curves_component.geometry.wrap();
        if (curves.is_empty()) {
          continue;
        }
        input.point_components.append(component_i);
        input.point_offsets.append(curves.evaluated_points_num());

        int total_segment_num = 0;
        const VArray<bool> &cyclic = curves.cyclic();
        const OffsetIndices<int> points_by_curve = curves.evaluated_points_by_curve();
        for (const int curve : curves.curves_range()) {
          total_segment_num += bke::curves::segments_num(points_by_curve[curve].size(),
                                                         cyclic[curve]);
        }
        input.edge_components.append(component_i);
        input.edge_offsets.append(total_segment_num);
        break;
      }
      case GeometryComponent::Type::Mesh: {
        const Mesh &mesh = *static_cast<const MeshComponent *>(component)->get();
        if (mesh.verts_num > 0) {
          input.point_components.append(component_i);
          input.point_offsets.append(mesh.verts_num);
        }
        if (mesh.loose_edges().count > 0) {
          input.edge_components.append(component_i);
          input.edge_offsets.append(mesh.loose_edges().count);
        }
        if (mesh.faces_num > 0) {
          input.face_components.append(component_i);
          input.face_offsets.append(mesh.faces_num);
        }
        break;
      }
      default:
        break;
    }
  }

  if (input.point_components.is_empty()) {
    return std::nullopt;
  }

  input.point_offsets.append(0);
  input.edge_offsets.append(0);
  input.face_offsets.append(0);
  const OffsetIndices<int> points_by_component = offset_indices::accumulate_counts_to_offsets(
      input.point_offsets);
  const OffsetIndices<int> edges_by_component = offset_indices::accumulate_counts_to_offsets(
      input.edge_offsets);
  const OffsetIndices<int> faces_by_component = offset_indices::accumulate_counts_to_offsets(
      input.face_offsets);

  meshintersect::CDT_input<double> &cdt_input = input.cdt_input;
  cdt_input.vert.reinitialize(points_by_component.total_size());
  cdt_input.edge.reinitialize(edges_by_component.total_size());
  cdt_input.face.reinitialize(faces_by_component.total_size());

  /** Add 2D points. */
  for (const int point_component_i : input.point_components.index_range()) {
    const int component_i = input.point_components[point_component_i];
    const GeometryComponent *component = all_components[component_i];
    const IndexRange dst_point_range = points_by_component[point_component_i];

    MutableSpan<double2> dst_positions_2d = cdt_input.vert.as_mutable_span().slice(
        dst_point_range);
    switch (component->type()) {
      case GeometryComponent::Type::PointCloud: {
        const PointCloud &pointcloud = *static_cast<const PointCloudComponent *>(component)->get();
        copy_positions_float3_to_double2(pointcloud.positions(), dst_positions_2d);
        break;
      }
      case GeometryComponent::Type::Curve: {
        const Curves &curves_component = *static_cast<const CurveComponent *>(component)->get();
        const bke::CurvesGeometry &curves = curves_component.geometry.wrap();
        copy_positions_float3_to_double2(curves.evaluated_positions(), dst_positions_2d);
        break;
      }
      case GeometryComponent::Type::Mesh: {
        const Mesh &mesh = *static_cast<const MeshComponent *>(component)->get();
        copy_positions_float3_to_double2(mesh.vert_positions(), dst_positions_2d);
        break;
      }
      default:
        break;
    }
  }

  /* Add edge constraints. */
  for (const int edge_component_i : input.edge_components.index_range()) {
    const int component_i = input.edge_components[edge_component_i];
    const GeometryComponent *component = all_components[component_i];
    const IndexRange dst_points_range =
        points_by_component[input.point_components.first_index_of(component_i)];
    const IndexRange dst_edge_range = edges_by_component[edge_component_i];

    MutableSpan<std::pair<int, int>> dst_edges = cdt_input.edge.as_mutable_span().slice(
        dst_edge_range);
    switch (component->type()) {
      case GeometryComponent::Type::Curve: {
        const Curves &curves_component = *static_cast<const CurveComponent *>(component)->get();
        const bke::CurvesGeometry &curves = curves_component.geometry.wrap();
        const VArray<bool> &cyclic = curves.cyclic();
        const OffsetIndices<int> points_by_curve = curves.evaluated_points_by_curve();

        Array<int> segment_offsets(curves.curves_num() + 1);
        threading::parallel_for(curves.curves_range(), 1024, [&](const IndexRange range) {
          for (const int curve : range) {
            segment_offsets[curve] = bke::curves::segments_num(points_by_curve[curve].size(),
                                                               cyclic[curve]);
          }
        });
        const OffsetIndices<int> edges_by_curve = offset_indices::accumulate_counts_to_offsets(
            segment_offsets.as_mutable_span());

        const int dst_points_start_offset = dst_points_range.start();
        threading::parallel_for(curves.curves_range(), 1024, [&](const IndexRange range) {
          for (const int curve : range) {
            const bool is_cyclic = cyclic[curve];
            const IndexRange points = points_by_curve[curve];
            const IndexRange dst_points = points.shift(dst_points_start_offset);
            MutableSpan<std::pair<int, int>> dst_edges_by_curve = dst_edges.slice(
                edges_by_curve[curve]);
            BLI_assert(bke::curves::segments_num(points.size(), is_cyclic) ==
                       dst_edges_by_curve.size());
            for (const int i : points.index_range().drop_back(1)) {
              dst_edges_by_curve[i] = {dst_points[i], dst_points[i] + 1};
            }
            if (is_cyclic && points.size() > 1) {
              dst_edges_by_curve.last() = {dst_points.last(), dst_points.first()};
            }
          }
        });
        break;
      }
      case GeometryComponent::Type::Mesh: {
        const Mesh &mesh = *static_cast<const MeshComponent *>(component)->get();
        const Span<int2> edges = mesh.edges();
        const int dst_points_start_offset = dst_points_range.start();

        IndexMaskMemory memory;
        const IndexMask loose_edges = IndexMask::from_bits(mesh.loose_edges().is_loose_bits,
                                                           memory);
        loose_edges.foreach_index(GrainSize(4096), [&](const int index, const int pos) {
          dst_edges[pos] = {edges[index].x + dst_points_start_offset,
                            edges[index].y + dst_points_start_offset};
        });
        break;
      }
      default:
        break;
    }
  }

  for (const int face_component_i : input.face_components.index_range()) {
    const int component_i = input.face_components[face_component_i];
    const GeometryComponent *component = all_components[component_i];
    const IndexRange dst_points_range =
        points_by_component[input.point_components.first_index_of(component_i)];
    const IndexRange dst_face_range = faces_by_component[face_component_i];

    MutableSpan<Vector<int>> dst_faces = cdt_input.face.as_mutable_span().slice(dst_face_range);
    switch (component->type()) {
      case GeometryComponent::Type::Mesh: {
        const Mesh &mesh = *static_cast<const MeshComponent *>(component)->get();
        const OffsetIndices faces = mesh.faces();
        const Span<int> corner_verts = mesh.corner_verts();

        const int dst_points_start_offset = dst_points_range.start();
        threading::parallel_for(faces.index_range(), 4096, [&](const IndexRange range) {
          for (const int i : range) {
            const IndexRange face = faces[i];
            dst_faces[i] = corner_verts.slice(face);
            for (int &dst_point : dst_faces[i]) {
              dst_point += dst_points_start_offset;
            }
          }
        });
        break;
      }
      default:
        break;
    }
  }

  return input;
}

static Vector<CDTGeometrySetInput> cdt_inputs_from_groups(const GeometrySet &geometry_set,
                                                          const Field<int> &group_index,
                                                          const AttributeFilter &attribute_filter)
{
  Vector<const GeometryComponent *> all_components = geometry_set.get_components();
  Vector<const GeometryComponent *> components;
  for (const GeometryComponent *component : all_components) {
    if (!ELEM(component->type(),
              bke::GeometryComponent::Type::PointCloud,
              bke::GeometryComponent::Type::Curve,
              // bke::GeometryComponent::Type::GreasePencil, /* TODO! */
              bke::GeometryComponent::Type::Mesh))
    {
      continue;
    }
    components.append(component);
  }

  if (components.is_empty()) {
    return {};
  }

  Array<std::optional<FieldEvaluator>> field_evaluators(components.size());
  Array<VArray<int>> group_ids_by_component(components.size());
  for (const int component_i : components.index_range()) {
    const GeometryComponent *component = components[component_i];
    bke::GeometryFieldContext field_context{*component, bke::AttrDomain::Point};
    const int point_num = component->attribute_domain_size(bke::AttrDomain::Point);
    FieldEvaluator &field_evaluator = field_evaluators[component_i].emplace(field_context,
                                                                            point_num);
    field_evaluator.add(group_index);
    field_evaluator.evaluate();

    VArray<int> group_ids = field_evaluator.get_evaluated<int>(0);
    group_ids_by_component[component_i] = std::move(group_ids);
  }

  bool is_single_group = false;
  std::optional<int> first_single_group_id = group_ids_by_component.first().get_if_single();
  if (first_single_group_id) {
    is_single_group = true;
    for (const VArray<int> &group_ids : group_ids_by_component.as_span().drop_front(1)) {
      std::optional<int> id = group_ids.get_if_single();
      if (!id || first_single_group_id != id) {
        is_single_group = false;
        break;
      }
    }
  }

  Vector<CDTGeometrySetInput> inputs;
  if (is_single_group) {
    std::optional<CDTGeometrySetInput> input = cdt_input_from_geometry_set(geometry_set);
    if (input) {
      inputs.append(std::move(*input));
    }
    return inputs;
  }

  IndexMaskMemory memory;
  Array<Vector<IndexMask>> group_id_masks_by_component(components.size());
  Array<VectorSet<int>> group_id_set_by_component(components.size());
  for (const int component_i : components.index_range()) {
    group_id_masks_by_component[component_i] = IndexMask::from_group_ids(
        group_ids_by_component[component_i], memory, group_id_set_by_component[component_i]);
  }

  VectorSet<int> all_group_ids;
  for (const int component_i : components.index_range()) {
    all_group_ids.add_multiple(group_id_set_by_component[component_i]);
  }

  Vector<GeometrySet> geometry_set_by_group_id(all_group_ids.size());
  for (const int geometry_i : geometry_set_by_group_id.index_range()) {
    GeometrySet &geometry_group = geometry_set_by_group_id[geometry_i];
    const int group_id = all_group_ids[geometry_i];

    for (const int component_i : components.index_range()) {
      const GeometryComponent *component = components[component_i];
      const int group_index = group_id_set_by_component[component_i].index_of_try(group_id);
      if (group_index == -1) {
        continue;
      }
      const Span<IndexMask> group_id_masks = group_id_masks_by_component[component_i];
      const IndexMask &group_id_mask = group_id_masks[group_index];
      switch (component->type()) {
        case GeometryComponent::Type::PointCloud: {
          const PointCloud &pointcloud =
              *static_cast<const PointCloudComponent *>(component)->get();
          PointCloud *dst_pointcloud = bke::pointcloud::copy_selection(
              pointcloud, group_id_mask, attribute_filter);
          geometry_group.replace_pointcloud(dst_pointcloud);
          break;
        }
        case GeometryComponent::Type::Curve: {
          const Curves &curves_component = *static_cast<const CurveComponent *>(component)->get();
          const bke::CurvesGeometry &curves = curves_component.geometry.wrap();
          Curves *dst_curves = bke::curves_new_nomain(
              bke::curves_copy_point_selection(curves, group_id_mask, attribute_filter));
          geometry_group.replace_curves(dst_curves);
          break;
        }
        case GeometryComponent::Type::Mesh: {
          const Mesh &mesh = *static_cast<const MeshComponent *>(component)->get();
          Array<bool> selection(mesh.verts_num);
          group_id_mask.to_bools(selection.as_mutable_span());
          Mesh *dst_mesh = *geometry::mesh_copy_selection(
              mesh,
              VArray<bool>::from_span(selection.as_span()),
              bke::AttrDomain::Point,
              attribute_filter);
          geometry_group.replace_mesh(dst_mesh);
          break;
        }
        default:
          break;
      }
    }
  }

  for (const GeometrySet &geometry_group : geometry_set_by_group_id) {
    std::optional<CDTGeometrySetInput> input = cdt_input_from_geometry_set(geometry_group);
    if (input) {
      inputs.append(std::move(*input));
    }
  }

  return inputs;
}


static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  Field<int> group_index = params.extract_input<Field<int>>("Group ID");

  const TriangulationMode mode = params.extract_input<TriangulationMode>("Mode");
  const CDT_output_type output_type = get_cdt_output_type(mode);

  const AttributeFilter &attribute_filter = params.get_attribute_filter("Mesh");

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    Vector<CDTGeometrySetInput> cdt_inputs_by_group = cdt_inputs_from_groups(
        geometry_set, group_index, attribute_filter);
    Array<meshintersect::CDT_result<double>> results = calculate_cdts(cdt_inputs_by_group,
                                                                      output_type);
    Mesh *mesh = geometry::cdts_to_mesh(results.as_span());
    geometry_set.replace_mesh(mesh);
    geometry_set.keep_only_during_modify({GeometryComponent::Type::Mesh});
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeDelaunayTriangulation");
  ntype.ui_name = "Delaunay Triangulation";
  ntype.ui_description =
      "Generate a triangulated mesh from a set of points in the X-Y plane. Uses edges and faces "
      "as triangulation constraints";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;

  blender::bke::node_type_size(ntype, 160, 140, NODE_DEFAULT_MAX_WIDTH);
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_delaunay_triangulation_cc
