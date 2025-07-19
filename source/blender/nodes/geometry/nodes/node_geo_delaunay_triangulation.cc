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
#include "GEO_mesh_selection.hh"

#include "NOD_rna_define.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_delaunay_triangulation_cc {

enum class TriangulationMode : int8_t {
  Full = 0,
  Inside = 1,
  InsideWidthHoles = 2,
};

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry")
      .supported_type({GeometryComponent::Type::Mesh,
                       GeometryComponent::Type::Curve,
                       GeometryComponent::Type::GreasePencil,
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
  b.add_output<decl::Geometry>("Mesh").propagate_all_instance_attributes();
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "mode", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = int(TriangulationMode::Full);
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

static Array<meshintersect::CDT_result<double>> calculate_cdts(
    const Span<meshintersect::CDT_input<double>> inputs, const CDT_output_type output_type)
{
  Array<meshintersect::CDT_result<double>> outputs(inputs.size());
  /* TODO: Use better grain size. */
  threading::parallel_for(inputs.index_range(), 8, [&](const IndexRange range) {
    for (const int i : range) {
      outputs[i] = meshintersect::delaunay_2d_calc(inputs[i], output_type);
    }
  });

  return outputs;
}

// static Array<meshintersect::CDT_result<double>> triangulate_points(GeometrySet &geometry_set,
//                                                                    const TriangulationMode mode)
// {
//   const PointCloud *pointcloud = geometry_set.get_pointcloud();
//   const Span<float3> positions = pointcloud->positions();

//   Array<double2> positions_2d(positions.size());
//   threading::parallel_for(positions.index_range(), 8196, [&](const IndexRange range) {
//     for (const int point : range) {
//       positions_2d[point] = double2(positions[point].xy());
//     }
//   });

//   meshintersect::CDT_input<double> input;
//   input.need_ids = true;
//   input.vert = std::move(positions_2d);

//   return {do_single_cdt(input, mode)};
// }

// static Array<meshintersect::CDT_result<double>> do_mesh_cdt(const Mesh &mesh,
//                                                             const Field<int> &group_index,
//                                                             const CDT_output_type output_type)
// {
//   // const bke::GeometryFieldContext field_context{mesh, AttrDomain::Point};
//   // fn::FieldEvaluator evaluator{field_context, mesh.verts_num};
//   // evaluator.add(group_index);
//   // evaluator.evaluate();
//   // const VArray<int> group_ids = evaluator.get_evaluated<int>(0);

//   const Span<float3> positions = mesh.vert_positions();
//   Array<double2> positions_2d(positions.size());
//   threading::parallel_for(positions.index_range(), 8196, [&](const IndexRange range) {
//     for (const int point : range) {
//       positions_2d[point] = double2(positions[point].xy());
//     }
//   });

//   meshintersect::CDT_input<double> input;
//   input.need_ids = false;
//   input.vert = std::move(positions_2d);

//   /* Edges that are connected to a face are automatically added. So only add loose edges. */
//   if (mesh.loose_edges().count > 0) {
//     const Span<int2> edges = mesh.edges();
//     Array<std::pair<int, int>> input_edges(mesh.loose_edges().count);

//     IndexMaskMemory memory;
//     const IndexMask loose_edges = IndexMask::from_bits(mesh.loose_edges().is_loose_bits,
//     memory); loose_edges.foreach_index(GrainSize(4096), [&](const int index, const int pos) {
//       input_edges[pos] = {edges[index].x, edges[index].y};
//     });

//     input.edge = std::move(input_edges);
//   }

//   if (!mesh.faces().is_empty()) {
//     const OffsetIndices faces = mesh.faces();
//     const Span<int> corner_verts = mesh.corner_verts();
//     Array<Vector<int>> input_faces(faces.size());

//     threading::parallel_for(faces.index_range(), 4096, [&](const IndexRange range) {
//       for (const int i : range) {
//         const IndexRange face = faces[i];
//         input_faces[i] = corner_verts.slice(face);
//       }
//     });

//     input.face = std::move(input_faces);
//   }
//   else {
//     /* If there are no faces, treat everything as one face. */
//     Array<Vector<int>> input_faces(1);
//     input_faces[0].reinitialize(positions.size());
//     array_utils::fill_index_range(input_faces[0].as_mutable_span());
//     input.face = std::move(input_faces);
//   }

//   return {delaunay_2d_calc(input, output_type)};
// }

struct CDTGeometrySetInput {
  meshintersect::CDT_input<double> cdt_input;
  Vector<const GeometryComponent *> all_components;
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

static void copy_positions_2d(const Span<float3> src_positions, MutableSpan<double2> dst_positions)
{
  threading::parallel_for(src_positions.index_range(), 4096, [&](const IndexRange range) {
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
  input.all_components = geometry_set.get_components();
  for (const int component_i : input.all_components.index_range()) {
    const GeometryComponent *component = input.all_components[component_i];
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
    const GeometryComponent *component = input.all_components[component_i];
    const IndexRange dst_point_range = points_by_component[point_component_i];

    MutableSpan<double2> dst_positions_2d = cdt_input.vert.as_mutable_span().slice(
        dst_point_range);
    switch (component->type()) {
      case GeometryComponent::Type::PointCloud: {
        const PointCloud &pointcloud = *static_cast<const PointCloudComponent *>(component)->get();
        copy_positions_2d(pointcloud.positions(), dst_positions_2d);
        break;
      }
      case GeometryComponent::Type::Curve: {
        const Curves &curves_component = *static_cast<const CurveComponent *>(component)->get();
        const bke::CurvesGeometry &curves = curves_component.geometry.wrap();
        copy_positions_2d(curves.evaluated_positions(), dst_positions_2d);
        break;
      }
      case GeometryComponent::Type::Mesh: {
        const Mesh &mesh = *static_cast<const MeshComponent *>(component)->get();
        copy_positions_2d(mesh.vert_positions(), dst_positions_2d);
        break;
      }
      default:
        break;
    }
  }

  /* Add edge constraints. */
  for (const int edge_component_i : input.edge_components.index_range()) {
    const int component_i = input.edge_components[edge_component_i];
    const GeometryComponent *component = input.all_components[component_i];
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

        Array<int> segment_offsets(curves.curves_num() + 1, 0);
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
    const GeometryComponent *component = input.all_components[component_i];
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

struct CDTGeometryInput {
  /* A span of all the 3d positions of this input geometry. */
  Span<float3> positions;
  Span<int2> edges;
  OffsetIndices<int> faces;
  Span<int> corner_verts;

  /* Group IDs used by this geometry. */
  VectorSet<int> group_ids;
  /* An index mask of points for each group ID in the geometry. */
  Vector<IndexMask> point_masks;
  /* An index mask of edges for each group ID in the geometry. */
  Vector<IndexMask> edge_masks;
  /* An index mask of faces for each group ID in the geometry. */
  Vector<IndexMask> face_masks;

  Vector<Map<int, int>> src_to_dst_point_maps;
};

static Vector<meshintersect::CDT_input<double>> construct_cdt_inputs(
    const GeometrySet &geometry_set, const Field<int> &group_index)
{
  IndexMaskMemory memory;

  Vector<CDTGeometryInput, 4> geometry_inputs;
  Vector<const GeometryComponent *> components = geometry_set.get_components();
  for (const GeometryComponent *component : components) {
    if (!ELEM(component->type(),
              bke::GeometryComponent::Type::PointCloud,
              bke::GeometryComponent::Type::Curve,
              bke::GeometryComponent::Type::GreasePencil, /* TODO! */
              bke::GeometryComponent::Type::Mesh))
    {
      continue;
    }
    bke::GeometryFieldContext field_context{*component, bke::AttrDomain::Point};
    const int point_num = component->attribute_domain_size(bke::AttrDomain::Point);
    FieldEvaluator field_evaluator{field_context, point_num};
    field_evaluator.add(group_index);
    field_evaluator.evaluate();

    CDTGeometryInput geometry_input;
    geometry_input.point_masks = IndexMask::from_group_ids(
        field_evaluator.get_evaluated<int>(0), memory, geometry_input.group_ids);
    const int num_group_ids = geometry_input.group_ids.size();

    switch (component->type()) {
      case bke::GeometryComponent::Type::PointCloud: {
        const PointCloud *pointcloud = geometry_set.get_pointcloud();
        geometry_input.positions = pointcloud->positions();
        break;
      }
      case bke::GeometryComponent::Type::Mesh: {
        const Mesh *mesh = geometry_set.get_mesh();
        geometry_input.positions = mesh->vert_positions();
        geometry_input.edges = mesh->edges();
        geometry_input.faces = mesh->faces();
        geometry_input.corner_verts = mesh->corner_verts();

        Vector<IndexMask> edge_masks(num_group_ids);
        Vector<IndexMask> face_masks(num_group_ids);
        for (const int group_index : IndexRange(num_group_ids)) {
          Array<bool> point_selection(mesh->verts_num);
          const IndexMask points = geometry_input.point_masks[group_index];
          points.to_bools(point_selection.as_mutable_span());

          edge_masks[group_index] = std::move(geometry::edge_selection_from_vert(
              mesh->edges(), point_selection.as_span(), memory));
          face_masks[group_index] = std::move(geometry::face_selection_from_vert(
              mesh->faces(), mesh->corner_verts(), point_selection.as_span(), memory));
        }
        geometry_input.edge_masks = std::move(edge_masks);
        geometry_input.face_masks = std::move(face_masks);

        geometry_input.src_to_dst_point_maps.reinitialize(num_group_ids);
        break;
      }
      default:
        break;
    }
    geometry_inputs.append_as(std::move(geometry_input));
  }

  VectorSet<int> all_group_ids;
  for (const CDTGeometryInput &geometry_input : geometry_inputs) {
    all_group_ids.add_multiple(geometry_input.group_ids);
  }

  // /* Create one CDT input per group ID. */
  // Vector<meshintersect::CDT_input<double>> cdt_inputs(all_group_ids.size());
  // for (const int input_i : cdt_inputs.index_range()) {
  //   const int group_id = all_group_ids[input_i];

  //   Vector<int> point_counts, edge_counts, face_counts;
  //   Vector<int> geometries_with_edges, geometries_with_faces, geometries_with_edges_or_faces;
  //   Vector<CDTGeometryInput *> geometry_inputs_for_group;
  //   for (const int geometry_i : geometry_inputs.index_range()) {
  //     CDTGeometryInput &geometry_input = geometry_inputs[geometry_i];
  //     const int group_index = geometry_input.group_ids.index_of_try(group_id);
  //     if (group_index == -1) {
  //       continue;
  //     }
  //     point_counts.append(geometry_input.point_masks[group_index].size());
  //     if (!geometry_input.edge_masks.is_empty()) {
  //       edge_counts.append(geometry_input.edge_masks[group_index].size());
  //       geometries_with_edges.append(geometry_inputs_for_group.size());
  //       geometries_with_edges_or_faces.append(geometry_inputs_for_group.size());
  //     }
  //     if (!geometry_input.face_masks.is_empty()) {
  //       face_counts.append(geometry_input.face_masks[group_index].size());
  //       geometries_with_faces.append(geometry_inputs_for_group.size());
  //       geometries_with_edges_or_faces.append(geometry_inputs_for_group.size());
  //     }
  //     geometry_inputs_for_group.append(&geometry_input);
  //   }
  //   BLI_assert(!geometry_inputs_for_group.is_empty());

  //   /* Append one more count so we can accumulate the counts to offsets inplace. */
  //   point_counts.append(0);
  //   edge_counts.append(0);
  //   face_counts.append(0);
  //   OffsetIndices<int> dst_points_by_geometry = offset_indices::accumulate_counts_to_offsets(
  //       point_counts.as_mutable_span());
  //   OffsetIndices<int> dst_edges_by_geometry, dst_faces_by_geometry;
  //   if (!geometries_with_edges.is_empty()) {
  //     dst_edges_by_geometry = offset_indices::accumulate_counts_to_offsets(
  //         edge_counts.as_mutable_span());
  //   }
  //   if (!geometries_with_faces.is_empty()) {
  //     dst_faces_by_geometry = offset_indices::accumulate_counts_to_offsets(
  //         face_counts.as_mutable_span());
  //   }

  //   /* Compute src to dst maps for later lookups. */
  //   if (!geometries_with_edges_or_faces.is_empty()) {
  //     for (const int geometry_i : geometries_with_edges_or_faces) {
  //       CDTGeometryInput &geometry_input = *geometry_inputs_for_group[geometry_i];
  //       const IndexRange dst_points_range = dst_points_by_geometry[geometry_i];

  //       const int group_index = geometry_input.group_ids.index_of(group_id);
  //       const IndexMask src_points = geometry_input.point_masks[group_index];
  //       Map<int, int> &src_to_dst_point_map = geometry_input.src_to_dst_point_maps[group_index];
  //       src_points.foreach_index([&](const int64_t src_point, const int64_t pos) {
  //         const int64_t dst_point = pos + dst_points_range.start();
  //         src_to_dst_point_map.add(src_point, dst_point);
  //       });
  //     }
  //   }

  //   const int total_verts = dst_points_by_geometry.total_size();
  //   const int total_edges = dst_edges_by_geometry.total_size();
  //   const int total_faces = dst_faces_by_geometry.total_size();
  //   BLI_assert(total_verts > 0);

  //   meshintersect::CDT_input<double> &cdt_input = cdt_inputs[input_i];
  //   cdt_input.vert.reinitialize(total_verts);
  //   cdt_input.edge.reinitialize(total_edges);
  //   cdt_input.face.reinitialize(total_faces);
  //   /* This will enable the computation of the mappings from dst to src vert indices. */
  //   cdt_input.need_ids = true;

  //   MutableSpan<double2> positions_2d = cdt_input.vert.as_mutable_span();
  //   for (const int geometry_i : geometry_inputs_for_group.index_range()) {
  //     const CDTGeometryInput &geometry_input = *geometry_inputs_for_group[geometry_i];
  //     const int group_index = geometry_input.group_ids.index_of(group_id);
  //     const IndexMask src_points = geometry_input.point_masks[group_index];
  //     const Span<float3> positions = geometry_input.positions;

  //     const IndexRange dst_points_range = dst_points_by_geometry[geometry_i];
  //     MutableSpan<double2> dst_positions = positions_2d.slice(dst_points_range);
  //     src_points.foreach_index(GrainSize(4096), [&](const int64_t src_point, const int64_t pos)
  //     {
  //       dst_positions[pos] = double2(positions[src_point].xy());
  //     });
  //   }

  //   MutableSpan<std::pair<int, int>> input_edges = cdt_input.edge.as_mutable_span();
  //   for (const int geometry_i : geometries_with_edges) {
  //     const CDTGeometryInput &geometry_input = *geometry_inputs_for_group[geometry_i];
  //     const int group_index = geometry_input.group_ids.index_of(group_id);
  //     const IndexMask src_edges = geometry_input.edge_masks[group_index];
  //     const Map<int, int> &src_to_dst_point_map =
  //         geometry_input.src_to_dst_point_maps[group_index];
  //     const Span<int2> edges = geometry_input.edges;

  //     const IndexRange dst_edges_range = dst_edges_by_geometry[geometry_i];
  //     MutableSpan<std::pair<int, int>> dst_edges = input_edges.slice(dst_edges_range);
  //     src_edges.foreach_index(GrainSize(4096), [&](const int64_t src_edge, const int64_t pos) {
  //       const int2 edge = edges[src_edge];
  //       BLI_assert(src_to_dst_point_map.contains(edge[0]) &&
  //                  src_to_dst_point_map.contains(edge[1]));
  //       dst_edges[pos] = {src_to_dst_point_map.lookup(edge[0]),
  //                         src_to_dst_point_map.lookup(edge[1])};
  //     });
  //   }

  //   MutableSpan<Vector<int>> input_faces = cdt_input.face.as_mutable_span();
  //   for (const int geometry_i : geometries_with_faces) {
  //     const CDTGeometryInput &geometry_input = *geometry_inputs_for_group[geometry_i];
  //     const int group_index = geometry_input.group_ids.index_of(group_id);
  //     const IndexMask src_faces = geometry_input.face_masks[group_index];
  //     const Map<int, int> &src_to_dst_point_map =
  //         geometry_input.src_to_dst_point_maps[group_index];
  //     const OffsetIndices<int> faces = geometry_input.faces;
  //     const Span<int> corner_verts = geometry_input.corner_verts;

  //     const IndexRange dst_faces_range = dst_faces_by_geometry[geometry_i];
  //     MutableSpan<Vector<int>> dst_faces = input_faces.slice(dst_faces_range);
  //     src_faces.foreach_index(GrainSize(1024), [&](const int64_t src_face, const int64_t pos) {
  //       Vector<int> dst_face(faces[src_face].size());
  //       const Span<int> indices = corner_verts.slice(faces[src_face]);
  //       for (const int point : dst_face.index_range()) {
  //         const int src_point = indices[point];
  //         BLI_assert(src_to_dst_point_map.contains(src_point));
  //         dst_face[point] = src_to_dst_point_map.lookup(src_point);
  //       }
  //       dst_faces[pos] = std::move(dst_face);
  //     });
  //   }
  // }
  return cdt_inputs;
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry_set = params.extract_input<GeometrySet>("Geometry");
  Field<int> group_index = params.extract_input<Field<int>>("Group ID");

  const TriangulationMode mode = TriangulationMode(params.node().custom1);
  const CDT_output_type output_type = get_cdt_output_type(mode);

  geometry_set.modify_geometry_sets([&](GeometrySet &geometry_set) {
    // Vector<meshintersect::CDT_input<double>> inputs = construct_cdt_inputs(geometry_set,
    //                                                                        group_index);
    std::optional<CDTGeometrySetInput> input = cdt_input_from_geometry_set(geometry_set);
    if (!input) {
      return;
    }
    Vector<meshintersect::CDT_input<double>> inputs{input->cdt_input};
    Array<meshintersect::CDT_result<double>> results = calculate_cdts(inputs, output_type);
    Mesh *mesh = geometry::cdts_to_mesh(results.as_span());
    geometry_set.replace_mesh(mesh);
    geometry_set.keep_only_during_modify({GeometryComponent::Type::Mesh});
  });

  params.set_output("Mesh", std::move(geometry_set));
}

static void node_rna(StructRNA *srna)
{
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

  RNA_def_node_enum(srna,
                    "mode",
                    "Mode",
                    "Mode for constrained delaunay triangulation",
                    mode_items,
                    NOD_inline_enum_accessors(custom1));
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
  ntype.initfunc = node_init;
  ntype.declare = node_declare;
  blender::bke::node_type_size(ntype, 160, 140, NODE_DEFAULT_MAX_WIDTH);
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;

  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_delaunay_triangulation_cc
