/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_attribute.hh"
#include "BKE_attribute_enums.hh"
#include "BKE_attribute_filters.hh"
#include "BKE_attribute_math.hh"
#include "BKE_deform.hh"
#include "BKE_geometry_fields.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BLI_array_utils.hh"
#include "BLI_atomic_disjoint_set.hh"
#include "BLI_execution_mode.hh"
#include "BLI_index_mask.hh"
#include "BLI_kdtree.hh"
#include "BLI_kdtree_types.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "FN_field_evaluation.hh"
#include "GEO_mesh_copy_selection.hh"
#include "GEO_mesh_replace_faces.hh"
#include "GEO_mesh_selection.hh"

namespace blender::geometry {

static GroupedSpan<int> build_face_to_face_by_edge_map(const OffsetIndices<int> faces,
                                                       const Span<int> corner_edges,
                                                       const int edges_num,
                                                       Array<int> &r_offsets,
                                                       Array<int> &r_indices)
{
  Array<int> edge_to_face_offset_data;
  Array<int> edge_to_face_indices;
  const GroupedSpan<int> edge_to_face_map = bke::mesh::build_edge_to_face_map(
      faces, corner_edges, edges_num, edge_to_face_offset_data, edge_to_face_indices);
  const OffsetIndices<int> edge_to_face_offsets(edge_to_face_offset_data);

  r_offsets = Array<int>(faces.size() + 1, 0);
  threading::parallel_for(faces.index_range(), 4096, [&](const IndexRange range) {
    for (const int face_i : range) {
      for (const int edge : corner_edges.slice(faces[face_i])) {
        /* Subtract face itself from the number of faces connected to the edge. */
        r_offsets[face_i] += edge_to_face_offsets[edge].size() - 1;
      }
    }
  });
  const OffsetIndices<int> offsets = offset_indices::accumulate_counts_to_offsets(r_offsets);
  r_indices.reinitialize(offsets.total_size());

  threading::parallel_for(faces.index_range(), 1024, [&](IndexRange range) {
    for (const int face_i : range) {
      MutableSpan<int> neighbors = r_indices.as_mutable_span().slice(offsets[face_i]);
      if (neighbors.is_empty()) {
        continue;
      }
      int count = 0;
      for (const int edge : corner_edges.slice(faces[face_i])) {
        for (const int neighbor : edge_to_face_map[edge]) {
          if (neighbor != face_i) {
            neighbors[count] = neighbor;
            count++;
          }
        }
      }
    }
  });

  return {OffsetIndices<int>(r_offsets), r_indices.as_span()};
}

Mesh *replace_faces(const Mesh &base,
                    const fn::Field<bool> &selection_field,
                    const fn::Field<int> &indices_field,
                    const fn::Field<float> &height_field,
                    const Span<const Mesh *> meshes)
{
  const Span<float3> base_positions = base.vert_positions();
  const Span<int2> base_edges = base.edges();
  const OffsetIndices<int> base_faces = base.faces();
  const Span<int> base_corner_verts = base.corner_verts();
  const Span<int> base_corner_edges = base.corner_edges();
  const Span<float3> base_face_normals = base.face_normals();
  const Span<float3> base_corner_normals = base.corner_normals();
  const bke::MeshFieldContext field_context(base, bke::AttrDomain::Face);
  fn::FieldEvaluator field_evaluator(field_context, base.faces_num);
  field_evaluator.set_selection(selection_field);
  field_evaluator.add(indices_field);
  field_evaluator.add(height_field);
  field_evaluator.evaluate();
  IndexMask selection = field_evaluator.get_evaluated_selection_as_mask();
  const VArraySpan<int> indices = field_evaluator.get_evaluated<int>(0);
  const VArraySpan<float> heights = field_evaluator.get_evaluated<float>(1);
  IndexMaskMemory memory;
  selection = array_utils::indices_in_range(selection, indices, meshes.index_range(), memory);
  const IndexMask unselected_faces = selection.complement(IndexMask(base.faces_num), memory);

  Array<int> mesh_vert_nums(meshes.size());
  Array<int> mesh_face_nums(meshes.size());
  Array<int> mesh_edge_nums(meshes.size());
  Array<int> mesh_corner_nums(meshes.size());
  Array<Span<float3>> mesh_positions(meshes.size());
  Array<OffsetIndices<int>> mesh_faces(meshes.size());
  Array<Span<int>> mesh_corner_verts(meshes.size());
  Array<Span<int>> mesh_corner_edges(meshes.size());
  for (const int i : meshes.index_range()) {
    mesh_vert_nums[i] = meshes[i]->verts_num;
    mesh_edge_nums[i] = meshes[i]->edges_num;
    mesh_face_nums[i] = meshes[i]->faces_num;
    mesh_corner_nums[i] = meshes[i]->corners_num;
    mesh_positions[i] = meshes[i]->vert_positions();
    mesh_faces[i] = meshes[i]->faces();
    mesh_corner_verts[i] = meshes[i]->corner_verts();
    mesh_corner_edges[i] = meshes[i]->corner_edges();
  }

  Array<IndexMask> mesh_boundary_verts(meshes.size());
  for (const int i : meshes.index_range()) {
  }

  Array<int> verts_all_by_part_data(base_faces.size() + 1);
  array_utils::gather<int>(
      mesh_vert_nums, indices, selection, verts_all_by_part_data.as_mutable_span().drop_back(1));
  index_mask::masked_fill<int>(verts_all_by_part_data, 0, unselected_faces);
  const std::optional<OffsetIndices<int>> verts_all_by_part_opt =
      offset_indices::accumulate_counts_to_offsets_with_overflow_check(verts_all_by_part_data);
  if (!verts_all_by_part_opt) {
    return BKE_mesh_copy_for_eval(base);
  }
  const OffsetIndices<int> verts_by_part = *verts_all_by_part_opt;

  Array<int> edges_all_by_part_data(base_faces.size() + 1);
  array_utils::gather<int>(
      mesh_edge_nums, indices, selection, edges_all_by_part_data.as_mutable_span().drop_back(1));
  index_mask::masked_fill<int>(edges_all_by_part_data, 0, unselected_faces);
  const std::optional<OffsetIndices<int>> edges_all_by_part_opt =
      offset_indices::accumulate_counts_to_offsets_with_overflow_check(edges_all_by_part_data);
  if (!edges_all_by_part_opt) {
    return BKE_mesh_copy_for_eval(base);
  }
  const OffsetIndices<int> edges_all_by_part = *edges_all_by_part_opt;

  Array<int> faces_by_part_data(base_faces.size() + 1);
  array_utils::gather<int>(
      mesh_face_nums, indices, selection, faces_by_part_data.as_mutable_span().drop_back(1));
  index_mask::masked_fill<int>(faces_by_part_data, 0, unselected_faces);
  const OffsetIndices<int> faces_by_part = offset_indices::accumulate_counts_to_offsets(
      faces_by_part_data);

  Array<int> corners_by_part_data(base_faces.size() + 1);
  array_utils::gather<int>(
      mesh_corner_nums, indices, selection, corners_by_part_data.as_mutable_span().drop_back(1));
  index_mask::masked_fill<int>(corners_by_part_data, 0, unselected_faces);
  const OffsetIndices<int> corners_by_part = offset_indices::accumulate_counts_to_offsets(
      corners_by_part_data);
  const int new_corners_num = corners_by_part.total_size();

  const IndexMask unselected_verts = vert_selection_from_face(
      base_faces, unselected_faces, base_corner_verts, base.verts_num, memory);

  const IndexMask unselected_edges = edge_selection_from_face(
      base_faces, unselected_faces, base_corner_edges, base.edges_num, memory);

  const int unselected_corners_num = offset_indices::sum_group_sizes(base_faces, unselected_faces);

  Array<float3> positions(unselected_verts.size() + verts_by_part.total_size());
  array_utils::gather(base_positions,
                      unselected_verts,
                      positions.as_mutable_span().take_front(unselected_verts.size()));
  selection.foreach_index(
      [&](const int base_face_i) {
        const IndexRange base_face = base_faces[base_face_i];
        const Span<int> face_verts = base_corner_verts.slice(base_face);
        const float height = heights[base_face_i];
        const Span<float3> src_positions = mesh_positions[indices[base_face_i]];
        MutableSpan<float3> part_positions = positions.as_mutable_span().slice(
            verts_by_part[base_face_i]);
        if (face_verts.size() == 4) {
          for (const int i : part_positions.index_range()) {
            const float2 xy = src_positions[i].xy();
            const float z = src_positions[i].z;
            const float2 factor = (xy + 1.0f) * 0.5f;
            const float4 mix_factors((1.0f - factor.x) * (1.0f - factor.y),
                                     factor.x * (1.0f - factor.y),
                                     factor.x * factor.y,
                                     (1.0f - factor.x) * factor.y);
            const float3 new_face_interp = bke::attribute_math::mix4(
                mix_factors,
                base_positions[face_verts[0]],
                base_positions[face_verts[1]],
                base_positions[face_verts[2]],
                base_positions[face_verts[3]]);
            const float3 normal = bke::attribute_math::mix4(mix_factors,
                                                            base_corner_normals[base_face[0]],
                                                            base_corner_normals[base_face[1]],
                                                            base_corner_normals[base_face[2]],
                                                            base_corner_normals[base_face[3]]);
            const float3 new_position = new_face_interp + normal * height * z;
            part_positions[i] = new_position;
          }
        }
        else {
          const float3x3 face_to_2d = math::axis_dominant_to_m3(base_face_normals[base_face_i]);
          for (const int i : part_positions.index_range()) {
            const float2 xy = src_positions[i].xy();
            const float z = src_positions[i].z;
          }
        }
      },
      exec_mode::grain_size(128));

  Array<int> face_to_face_map_offsets;
  Array<int> face_to_face_map_indices;
  const GroupedSpan<int> base_face_to_face_map = build_face_to_face_by_edge_map(
      base_faces,
      base_corner_edges,
      base.edges_num,
      face_to_face_map_offsets,
      face_to_face_map_indices);

  BitVector<> selection_bits(base_faces.size());
  selection.to_bits(selection_bits);

  const float threshold = 1e-4f;
  const float threshold_sq = threshold * threshold;

  Array<int> base_vert_to_unselected(base.verts_num);
  index_mask::build_reverse_map<int>(unselected_verts, base_vert_to_unselected);

  // TODO: PROTECT AGAINST MERGING VERTICES IN THE SAME PART
  AtomicDisjointSet disjoint_set(positions.size());
  selection.foreach_index(
      [&](const int base_face_i) {
        const IndexRange part_verts = verts_by_part[base_face_i];
        const Span<float3> face_positions = positions.as_span().slice(part_verts);
        const Span<int> neighbor_faces = base_face_to_face_map[base_face_i];
        for (const int neighbor_face : neighbor_faces) {
          if (selection_bits[neighbor_face]) {
            const IndexRange neighbor_range = verts_by_part[neighbor_face];
            const Span<float3> neighbor_positions = mesh_positions[indices[neighbor_face]];

            // TODO: REPLACE QUADRATIC LOOP WITH ACCELERATION STRUCTURE

            for (const int i : face_positions.index_range()) {
              const float3 &position = face_positions[i];
              const int part_vert = part_verts[i];
              for (const int neighbor_i : neighbor_positions.index_range()) {
                const float3 &neighbor_position = neighbor_positions[neighbor_i];
                if (math::distance_squared(position, neighbor_position) > threshold_sq) {
                  continue;
                }
                const int vert_neighbor = neighbor_range[neighbor_i];
                disjoint_set.join(part_vert, vert_neighbor);
              }
            }
          }
          else {
            for (const int i : face_positions.index_range()) {
              const float3 &position = face_positions[i];
              const int part_vert = part_verts[i];
              const Span<int> neighbor_face_verts = base_corner_verts.slice(
                  base_faces[neighbor_face]);
              for (const int neighbor_vert : neighbor_face_verts) {
                const float3 &neighbor_position = base_positions[neighbor_vert];
                if (math::distance_squared(position, neighbor_position) > threshold_sq) {
                  continue;
                }
                const int vert_base_new = base_vert_to_unselected[neighbor_vert];
                disjoint_set.join(part_vert, vert_base_new);
              }
            }
          }
        }
      },
      exec_mode::grain_size(128));

  Array<int> merged_verts(positions.size());
  const int merged_verts_num = disjoint_set.calc_reduced_ids(merged_verts);

  // TODO: EDGE MERGING
  Array<int> merged_edges(positions.size());
  const int merged_edges_num = 0;

  Mesh *result = BKE_mesh_new_nomain(unselected_verts.size() + merged_verts_num,
                                     unselected_edges.size() + merged_edges_num,
                                     unselected_faces.size() + faces_by_part.total_size(),
                                     unselected_corners_num + corners_by_part.total_size());

  MutableSpan<float3> result_positions = result->vert_positions_for_write();
  array_utils::gather(
      base_positions, unselected_verts, result_positions.take_front(unselected_verts.size()));
  // TODO: AVERAGE POSITIONS
  array_utils::gather(
      positions.as_span(), merged_verts.as_span(), result_positions.take_back(merged_verts_num));

  // TODO: EDGES
  MutableSpan<int2> result_edges = result->edges_for_write();

  MutableSpan<int> result_face_offsets = result->face_offsets_for_write();
  if (!unselected_faces.is_empty()) {
    offset_indices::gather_selected_offsets(
        base_faces, unselected_faces, result_face_offsets.take_front(unselected_faces.size() + 1));
  }

  selection.foreach_index(
      [&](const int base_face_i) {
        const OffsetIndices<int> faces = mesh_faces[indices[base_face_i]];
        const IndexRange part_faces = faces_by_part[base_face_i].shift(unselected_faces.size());
        const int part_corner_start = unselected_corners_num +
                                      corners_by_part[base_face_i].start();
        offset_indices::gather_selected_offsets(
            faces,
            faces.index_range(),
            part_corner_start,
            result_face_offsets.slice(part_faces.start(), part_faces.size() + 1));
      },
      exec_mode::grain_size(128));
  const OffsetIndices<int> result_faces = result->faces();

  MutableSpan<int> result_corner_verts = result->corner_verts_for_write();
  MutableSpan<int> result_corner_edges = result->corner_edges_for_write();

  mesh_gather_elements_and_remap_verts(base_faces,
                                       result_faces,
                                       base_vert_to_unselected,
                                       unselected_edges,
                                       unselected_faces,
                                       base_edges,
                                       base_corner_verts,
                                       result_edges.take_front(unselected_edges.size()),
                                       result_corner_verts.take_front(unselected_corners_num));

  MutableSpan<int> new_corner_verts = result_corner_verts.take_back(new_corners_num);
  selection.foreach_index(
      [&](const int base_face_i) {
        array_utils::gather(merged_verts.as_span().slice(verts_by_part[base_face_i]),
                            mesh_corner_verts[indices[base_face_i]],
                            new_corner_verts.slice(corners_by_part[base_face_i]));
      },
      exec_mode::grain_size(512));

  // TODO: ONLY IF THE SELECTION ISN'T FULL
  Array<int> base_edge_to_unselected(base.edges_num);
  index_mask::build_reverse_map<int>(unselected_edges, base_edge_to_unselected);

  mesh_gather_elements_and_remap_edges(base_faces,
                                       result_faces,
                                       base_edge_to_unselected,
                                       unselected_faces,
                                       base_corner_edges,
                                       result_corner_edges);

  MutableSpan<int> new_corner_edges = result_corner_edges.take_back(new_corners_num);
  selection.foreach_index(
      [&](const int base_face_i) {
        array_utils::gather(merged_edges.as_span().slice(edges_all_by_part[base_face_i]),
                            mesh_corner_edges[indices[base_face_i]],
                            new_corner_edges.slice(corners_by_part[base_face_i]));
      },
      exec_mode::grain_size(512));

  bke::MutableAttributeAccessor result_attributes = result->attributes_for_write();
  base.attributes().foreach_attribute([&](const bke::AttributeIter &iter) {
    if (ELEM(iter.name, "position", ".edge_verts", ".corner_vert", ".corner_edge")) {
      return;
    }
    const bool any_part_has_attribute = false;
    if (any_part_has_attribute) {
    }
    else {
      const GVArray src_attr = *iter.get();
      const CommonVArrayInfo info = src_attr.common_info();
      if (info.type == CommonVArrayInfo::Type::Single) {
        const bke::AttributeInitValue init(GPointer(src_attr.type(), info.data));
        if (result_attributes.add(iter.name, iter.domain, iter.data_type, init)) {
          return;
        }
      }
      const GVArraySpan src_span = src_attr;
      switch (iter.domain) {
        case bke::AttrDomain::Point: {
          break;
        }
        case bke::AttrDomain::Edge: {
          break;
        }
        case bke::AttrDomain::Face: {
          bke::GSpanAttributeWriter dst_attr = result_attributes.lookup_or_add_for_write_only_span(
              iter.name, iter.domain, iter.data_type);
          array_utils::gather(src_attr, unselected_faces, dst_attr.span);
          bke::attribute_math::gather_to_groups(
              faces_by_part,
              selection,
              src_span,
              dst_attr.span.take_back(faces_by_part.total_size()));
          dst_attr.finish();
          break;
        }
        case bke::AttrDomain::Corner: {
          break;
        }
        default: {
          break;
        }
      }
    }
  });

  // Build the position of every single vertex on the new face meshes. For quads, it might be best
  // to do this with a transform per face. The transform should move 0,0,0 to the first corner of
  // the base face, and 1,1,0 to the third corner. Alternatively we coudl just always do quad
  // interpolation of the mesh vertices based on their location in the XY space. The height should
  // come from multiplying the mesh vertex Z position with a height input for the each selected
  // face. For N-gons, we can use the triangulation and do a UV style interpolation from the base
  // face location in the same triangulation of a standard N-gon shape to the new positions. The
  // height mixing would be the same.

  // With a disjoint set of all the vertices, including original vertices. The set will have to be
  // sized to include unselected original vertices too. For each face, find the neighboring base
  // face or new face meshes. For each neighbor edge, build a kdtree of the vertices in the face
  // and the neighboring vertices. Merge the vertices of neighbors within the threshold. We only
  // ever want to merge vertices of boundary edges with existing vertices of the base mesh or
  // boundary vertices of neighboring mesh parts; we should never merge vertices within a part.
  // This implies only the vertices that are candidates for merging should be added to the KDtree.

  // For building edges, it's crucial we don't use a VectorSet for the entire result mesh's edges.
  // Ideally we'd avoid nested containers with bad allocation patterns as well. We need to make
  // sure we don't duplicate an existing mesh edge, but we can do that by searching through the
  // neighboring edges of the base vertices we're connecting to, rather than building a full
  // VectorSet. Also we never need to deduplicate edges that are added in each part, because we
  // never merged vertices within a part.

  // Building faces is simple, we just copy over the faces from the part meshes, remapping the
  // vertex indices and setting the indices for the newly created edges or existing edges.

  // Attribute merging. When there is only a single input mesh this should end up as a single or
  // multiple calls to attribute_math::gather, i.e. just an index-based copy. That should also be
  // the case for attributes that only exist on one of the input meshes (i.e. no mixing is
  // necessary). For attributes that exist on multiple input meshes (including the base mesh), it
  // gets more complicated.

  return result;
}

}  // namespace blender::geometry
