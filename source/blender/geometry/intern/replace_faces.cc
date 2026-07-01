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
#include "BLI_disjoint_set.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_execution_mode.hh"
#include "BLI_index_mask.hh"
#include "BLI_kdtree.hh"
#include "BLI_kdtree_types.hh"
#include "BLI_math_geom.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_sort.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"
#include "FN_field_evaluation.hh"
#include "GEO_mesh_copy_selection.hh"
#include "GEO_mesh_replace_faces.hh"
#include "GEO_mesh_selection.hh"
#include <algorithm>

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

static float4 bilinear_mix_factors_from_xy(const float2 xy)
{
  const float2 factor = (xy + 1.0f) * 0.5f;
  return {
      (1.0f - factor.x) * (1.0f - factor.y),
      factor.x * (1.0f - factor.y),
      factor.x * factor.y,
      (1.0f - factor.x) * factor.y,
  };
}
static void interpolate_positions_quads(const Span<float3> base_positions,
                                        const OffsetIndices<int> base_faces,
                                        const Span<int> base_corner_verts,
                                        const Span<float3> base_corner_normals,
                                        const IndexMask &mask,
                                        const Span<int> indices,
                                        const Span<Span<float3>> mesh_positions,
                                        const Span<float> heights,
                                        const OffsetIndices<int> verts_by_part,
                                        MutableSpan<float3> positions)
{
  mask.foreach_index([&](const int base_face_i) {
    const IndexRange base_face = base_faces[base_face_i];
    const Span<int> face_verts = base_corner_verts.slice(base_face);
    const float height = heights[base_face_i];
    const Span<float3> src_positions = mesh_positions[indices[base_face_i]];
    MutableSpan<float3> part_positions = positions.slice(verts_by_part[base_face_i]);
    for (const int i : part_positions.index_range()) {
      const float z = src_positions[i].z;
      const float4 mix_factors = bilinear_mix_factors_from_xy(src_positions[i].xy());
      const float3 new_face_interp = bke::attribute_math::mix4(mix_factors,
                                                               base_positions[face_verts[0]],
                                                               base_positions[face_verts[1]],
                                                               base_positions[face_verts[2]],
                                                               base_positions[face_verts[3]]);
      const float3 normal = math::normalize(
          bke::attribute_math::mix4(mix_factors,
                                    base_corner_normals[base_face[0]],
                                    base_corner_normals[base_face[1]],
                                    base_corner_normals[base_face[2]],
                                    base_corner_normals[base_face[3]]));
      const float3 new_position = new_face_interp + normal * height * z;
      part_positions[i] = new_position;
    }
  });
}

static void interpolate_positions_ngons(const Span<float3> base_positions,
                                        const OffsetIndices<int> base_faces,
                                        const Span<int> base_corner_verts,
                                        const Span<float3> base_face_normals,
                                        const Span<float3> base_corner_normals,
                                        const Span<float> heights,
                                        const IndexMask &mask,
                                        const Span<int> indices,
                                        const Span<Span<float3>> mesh_positions,
                                        const OffsetIndices<int> verts_by_part,
                                        MutableSpan<float3> positions)
{
  mask.foreach_index(
      [&](const int base_face_i) {
        const IndexRange base_face = base_faces[base_face_i];
        const Span<int> face_verts = base_corner_verts.slice(base_face);
        const float height = heights[base_face_i];
        const Span<float3> src_positions = mesh_positions[indices[base_face_i]];
        MutableSpan<float3> part_positions = positions.slice(verts_by_part[base_face_i]);
        const float3x3 face_to_2d = math::axis_dominant_to_m3(base_face_normals[base_face_i]);
        for (const int i : part_positions.index_range()) {
          const float2 xy = src_positions[i].xy();
          const float z = src_positions[i].z;
        }
      },
      exec_mode::grain_size(128));
}

static int merge_verts(const Span<float3> base_positions,
                       const OffsetIndices<int> base_faces,
                       const Span<int> base_corner_verts,
                       const GroupedSpan<int> base_face_to_face_map,
                       const IndexMask &selection,
                       const Span<int> base_vert_to_unselected,
                       const OffsetIndices<int> verts_all_by_part,
                       const int unselected_verts_num,
                       const Span<float3> positions_all,
                       const float threshold_sq,
                       MutableSpan<int> merged_verts)
{
  BitVector<> selection_bits(base_faces.size());
  selection.to_bits(selection_bits);

  /* Find all candidate merges in parallel. Each candidate joins a vertex of the current part to a
   * vertex of a different part (or a base mesh vertex); vertices of the same part are not
   * compared. */
  threading::EnumerableThreadSpecific<Vector<int2>> candidates_by_thread;
  selection.foreach_index(
      [&](const int base_face_i) {
        Vector<int2> &candidates = candidates_by_thread.local();
        const IndexRange part_verts = verts_all_by_part[base_face_i].shift(unselected_verts_num);
        const Span<float3> face_positions = positions_all.slice(part_verts);
        const Span<int> neighbor_faces = base_face_to_face_map[base_face_i];
        for (const int neighbor_face : neighbor_faces) {
          if (selection_bits[neighbor_face]) {
            const IndexRange neighbor_range = verts_all_by_part[neighbor_face].shift(
                unselected_verts_num);
            const Span<float3> neighbor_positions = positions_all.slice(neighbor_range);

            // TODO: REPLACE QUADRATIC LOOP WITH ACCELERATION STRUCTURE

            for (const int i : face_positions.index_range()) {
              const float3 &position = face_positions[i];
              for (const int neighbor_i : neighbor_positions.index_range()) {
                if (math::distance_squared(position, neighbor_positions[neighbor_i]) >
                    threshold_sq) {
                  continue;
                }
                candidates.append(int2(part_verts[i], neighbor_range[neighbor_i]));
              }
            }
          }
          else {
            const Span<int> neighbor_face_verts = base_corner_verts.slice(
                base_faces[neighbor_face]);
            for (const int i : face_positions.index_range()) {
              const float3 &position = face_positions[i];
              for (const int neighbor_vert : neighbor_face_verts) {
                if (math::distance_squared(position, base_positions[neighbor_vert]) > threshold_sq)
                {
                  continue;
                }
                candidates.append(int2(part_verts[i], base_vert_to_unselected[neighbor_vert]));
              }
            }
          }
        }
      },
      exec_mode::grain_size(128));

  int merges = 0;
  for (const Vector<int2> &local : candidates_by_thread) {
    merges += local.size();
  }
  Vector<int2> candidates;
  candidates.reserve(merges);
  for (const Vector<int2> &local : candidates_by_thread) {
    candidates.extend_unchecked(local);
  }

  /* Sort so the result is deterministic. */
  parallel_sort(candidates,
                [](const int2 a, const int2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });

  /* Skip merges that would place two vertices of the same part - or two unselected base vertices -
   * in one group. Each group's membership is a per-root singly linked list of the part index for
   * part vertices, or -1 for every unselected base vertex, so a group can contain at most one of
   * each part and at most one base vertex. */
  Array<int> vert_parts(positions_all.size(), -1);
  selection.foreach_index(
      [&](const int base_face_i) {
        const IndexRange part_verts = verts_all_by_part[base_face_i].shift(unselected_verts_num);
        vert_parts.as_mutable_span().slice(part_verts).fill(base_face_i);
      },
      exec_mode::grain_size(512));

  Array<int> list_head(positions_all.size());
  Array<int> list_next(positions_all.size(), -1);
  threading::parallel_for(positions_all.index_range(), 4096, [&](const IndexRange range) {
    array_utils::fill_index_range<int>(list_head.as_mutable_span().slice(range), range.start());
  });

  const auto same_part = [&](const int root_a, const int root_b) {
    for (int a = list_head[root_a]; a != -1; a = list_next[a]) {
      for (int b = list_head[root_b]; b != -1; b = list_next[b]) {
        if (vert_parts[a] == vert_parts[b]) {
          return true;
        }
      }
    }
    return false;
  };

  DisjointSet<int> disjoint_set(positions_all.size());
  for (const int2 candidate : candidates) {
    const int root_a = disjoint_set.find_root(candidate.x);
    const int root_b = disjoint_set.find_root(candidate.y);
    if (root_a == root_b) {
      continue;
    }
    if (same_part(root_a, root_b)) {
      continue;
    }
    const int survivor = disjoint_set.join(candidate.x, candidate.y);
    const int absorbed = survivor == root_a ? root_b : root_a;
    /* Append the absorbed group's label list onto the surviving root's. */
    int tail = list_head[survivor];
    while (list_next[tail] != -1) {
      tail = list_next[tail];
    }
    list_next[tail] = list_head[absorbed];
  }

  return disjoint_set.calc_reduced_ids(merged_verts);
}

static int merge_edges(const Span<int2> base_edges,
                       const IndexMask &unselected_edges,
                       const Span<int> base_vert_to_unselected,
                       const IndexMask &selection,
                       const Span<int> indices,
                       const Span<Span<int2>> mesh_edges,
                       const OffsetIndices<int> verts_all_by_part,
                       const OffsetIndices<int> edges_all_by_part,
                       const Span<int> merged_verts,
                       const int merged_verts_num,
                       MutableSpan<int2> edges_merged,
                       MutableSpan<int> merged_edges)
{
  const int unselected_edges_num = unselected_edges.size();

  /* Canonicalize every edge to the pair of merged vertices it connects. The index space matches
   * the disjoint set and `merged_edges`: unselected base edges first, then part edges grouped by
   * part. Because the vertex ids are already the merged (result) vertex ids, `edges_merged` is
   * also the final result edge-vertex pair for each edge. Two edges are duplicates exactly when
   * they share this canonical vertex pair, so no geometric test or transitive closure is needed
   * here; the disjoint set only collapses the (possibly more than two) edges that share a pair. */
  unselected_edges.foreach_index(
      [&](const int edge, const int pos) {
        const int2 verts = base_edges[edge];
        edges_merged[pos] = int2(merged_verts[base_vert_to_unselected[verts[0]]],
                                 merged_verts[base_vert_to_unselected[verts[1]]]);
      },
      exec_mode::grain_size(4096));
  selection.foreach_index(
      [&](const int base_face_i) {
        const Span<int2> part_edges = mesh_edges[indices[base_face_i]];
        const int vert_offset = verts_all_by_part[base_face_i].start();
        const IndexRange part_edge_range = edges_all_by_part[base_face_i];
        MutableSpan<int2> dst = edges_merged.slice(unselected_edges_num + part_edge_range.start(),
                                                   part_edge_range.size());
        for (const int i : part_edges.index_range()) {
          const int2 verts = part_edges[i];
          dst[i] = int2(merged_verts[vert_offset + verts[0]],
                        merged_verts[vert_offset + verts[1]]);
        }
      },
      exec_mode::grain_size(128));

  /* Group edges by an incident merged vertex so that duplicates can be discovered locally and in
   * parallel, avoiding a global edge set. Keying on the merged vertex handles part-to-part,
   * part-to-base, and base-to-base merges uniformly. */
  Array<int> vert_to_edge_offsets;
  Array<int> vert_to_edge_indices;
  const GroupedSpan<int> vert_to_edge_map = bke::mesh::build_vert_to_edge_map(
      edges_merged, merged_verts_num, vert_to_edge_offsets, vert_to_edge_indices);

  AtomicDisjointSet disjoint_set(edges_merged.size());
  threading::parallel_for(IndexRange(merged_verts_num), 1024, [&](const IndexRange range) {
    for (const int vert : range) {
      const Span<int> incident_edges = vert_to_edge_map[vert];
      for (const int i : incident_edges.index_range()) {
        const int other_i = bke::mesh::edge_other_vert(edges_merged[incident_edges[i]], vert);
        for (const int j : incident_edges.index_range().drop_front(i + 1)) {
          const int other_j = bke::mesh::edge_other_vert(edges_merged[incident_edges[j]], vert);
          if (other_i == other_j) {
            disjoint_set.join(incident_edges[i], incident_edges[j]);
          }
        }
      }
    }
  });
  return disjoint_set.calc_reduced_ids(merged_edges);
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
  const IndexMask quads = IndexMask::from_predicate(
      selection, memory, [&](const int i) { return base_faces[i].size() == 4; });
  const IndexMask ngons = quads.complement(selection, memory);

  Array<int> mesh_vert_nums(meshes.size());
  Array<int> mesh_face_nums(meshes.size());
  Array<int> mesh_edge_nums(meshes.size());
  Array<int> mesh_corner_nums(meshes.size());
  Array<Span<float3>> mesh_positions(meshes.size());
  Array<Span<int2>> mesh_edges(meshes.size());
  Array<OffsetIndices<int>> mesh_faces(meshes.size());
  Array<Span<int>> mesh_corner_verts(meshes.size());
  Array<Span<int>> mesh_corner_edges(meshes.size());
  for (const int i : meshes.index_range()) {
    mesh_vert_nums[i] = meshes[i]->verts_num;
    mesh_edge_nums[i] = meshes[i]->edges_num;
    mesh_face_nums[i] = meshes[i]->faces_num;
    mesh_corner_nums[i] = meshes[i]->corners_num;
    mesh_positions[i] = meshes[i]->vert_positions();
    mesh_edges[i] = meshes[i]->edges();
    mesh_faces[i] = meshes[i]->faces();
    mesh_corner_verts[i] = meshes[i]->corner_verts();
    mesh_corner_edges[i] = meshes[i]->corner_edges();
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
  const OffsetIndices<int> verts_all_by_part = *verts_all_by_part_opt;

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

  Array<float3> positions_all(unselected_verts.size() + verts_all_by_part.total_size());
  array_utils::gather(base_positions,
                      unselected_verts,
                      positions_all.as_mutable_span().take_front(unselected_verts.size()));
  interpolate_positions_quads(
      base_positions,
      base_faces,
      base_corner_verts,
      base_corner_normals,
      quads,
      indices,
      mesh_positions,
      heights,
      verts_all_by_part,
      positions_all.as_mutable_span().take_back(verts_all_by_part.total_size()));
  interpolate_positions_ngons(
      base_positions,
      base_faces,
      base_corner_verts,
      base_face_normals,
      base_corner_normals,
      heights,
      ngons,
      indices,
      mesh_positions,
      verts_all_by_part,
      positions_all.as_mutable_span().take_back(verts_all_by_part.total_size()));

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

  Array<int> merged_verts(positions_all.size());
  const int merged_verts_num = merge_verts(base_positions,
                                           base_faces,
                                           base_corner_verts,
                                           base_face_to_face_map,
                                           selection,
                                           base_vert_to_unselected,
                                           verts_all_by_part,
                                           unselected_verts.size(),
                                           positions_all,
                                           threshold_sq,
                                           merged_verts);

  /* Full edge index space, matching `merged_verts`: unselected base edges, then part edges.
   * `edges_merged` holds each edge's result vertex pair; `merged_edges` maps to the result edge.
   */
  Array<int2> edges_merged(unselected_edges.size() + edges_all_by_part.total_size());
  Array<int> merged_edges(unselected_edges.size() + edges_all_by_part.total_size());
  const int merged_edges_num = merge_edges(base_edges,
                                           unselected_edges,
                                           base_vert_to_unselected,
                                           selection,
                                           indices,
                                           mesh_edges,
                                           verts_all_by_part,
                                           edges_all_by_part,
                                           merged_verts,
                                           merged_verts_num,
                                           edges_merged,
                                           merged_edges);

  /* The result uses the merged index space directly: vertex and edge ids are the reduced ids from
   * the disjoint sets, so every reference is just mapped through `merged_verts` / `merged_edges`.
   * Faces and corners keep the simple "unselected faces, then part faces" layout. */
  Mesh *result = BKE_mesh_new_nomain(merged_verts_num,
                                     merged_edges_num,
                                     unselected_faces.size() + faces_by_part.total_size(),
                                     unselected_corners_num + corners_by_part.total_size());

  /* Scatter every source position to its merged vertex. */
  // TODO: AVERAGE POSITIONS
  MutableSpan<float3> result_positions = result->vert_positions_for_write();
  array_utils::scatter(positions_all.as_span(), merged_verts.as_span(), result_positions);

  MutableSpan<int> result_face_offsets = result->face_offsets_for_write();
  if (!unselected_faces.is_empty()) {
    offset_indices::gather_selected_offsets(
        base_faces, unselected_faces, result_face_offsets.take_front(unselected_faces.size() + 1));
  }

  /* Copy face offsets from part meshes. */
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

  MutableSpan<int2> result_edges = result->edges_for_write();
  MutableSpan<int> result_corner_verts = result->corner_verts_for_write();
  MutableSpan<int> result_corner_edges = result->corner_edges_for_write();

  const int unselected_verts_num = unselected_verts.size();
  const int unselected_edges_num = unselected_edges.size();

  Array<int> base_edge_to_unselected(base.edges_num);
  index_mask::build_reverse_map<int>(unselected_edges, base_edge_to_unselected);

  /* Scatter each edge's merged vertex pair to its merged edge. Duplicate edges write the same
   * pair to the same index. */
  array_utils::scatter(edges_merged.as_span(), merged_edges.as_span(), result_edges);

  /* Corners of the unselected faces, remapped into the merged index space. Every vertex and edge
   * of an unselected face is itself unselected, so the reverse maps are always valid here. */
  unselected_faces.foreach_index(
      [&](const int base_face_i, const int pos) {
        const IndexRange src = base_faces[base_face_i];
        const IndexRange dst = result_faces[pos];
        for (const int i : src.index_range()) {
          result_corner_verts[dst[i]] =
              merged_verts[base_vert_to_unselected[base_corner_verts[src[i]]]];
          result_corner_edges[dst[i]] =
              merged_edges[base_edge_to_unselected[base_corner_edges[src[i]]]];
        }
      },
      exec_mode::grain_size(512));

  /* Corners of the part faces, mapped through this part's slice of the merged spaces. */
  selection.foreach_index(
      [&](const int base_face_i) {
        const Span<int> verts_map = merged_verts.as_span().slice(
            unselected_verts_num + verts_all_by_part[base_face_i].start(),
            verts_all_by_part[base_face_i].size());
        const Span<int> edges_map = merged_edges.as_span().slice(
            unselected_edges_num + edges_all_by_part[base_face_i].start(),
            edges_all_by_part[base_face_i].size());
        const IndexRange corners = corners_by_part[base_face_i];
        array_utils::gather(verts_map,
                            mesh_corner_verts[indices[base_face_i]],
                            result_corner_verts.take_back(new_corners_num).slice(corners));
        array_utils::gather(edges_map,
                            mesh_corner_edges[indices[base_face_i]],
                            result_corner_edges.take_back(new_corners_num).slice(corners));
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
      bke::GSpanAttributeWriter dst_attr = result_attributes.lookup_or_add_for_write_only_span(
          iter.name, iter.domain, iter.data_type);
      switch (iter.domain) {
        case bke::AttrDomain::Point: {
          // bke::attribute_math::to_static_type(src_attr.type(), [&]<typename T>() {
          //   if constexpr (!std::is_same_v<T, std::string>) {
          //     const Span<T> src_attr = src_span.typed<T>();
          //     MutableSpan<T> dst_attr = dst_attr.span.template typed<T>();
          //     quads.foreach_index([&](const int base_face_i) {
          //       // for ()
          //     });
          //   }
          // });
          // array_utils::gather(
          //     src_attr, unselected_verts, dst_attr.span.take_front(unselected_verts.size()));
          // TODO
          break;
        }
        case bke::AttrDomain::Edge: {
          // array_utils::gather(
          //     src_attr, unselected_edges, dst_attr.span.take_front(unselected_edges.size()));
          // TODO
          break;
        }
        case bke::AttrDomain::Face: {
          array_utils::gather(
              src_attr, unselected_faces, dst_attr.span.take_front(unselected_faces.size()));
          bke::attribute_math::gather_to_groups(
              faces_by_part,
              selection,
              src_span,
              dst_attr.span.take_back(faces_by_part.total_size()));
          break;
        }
        case bke::AttrDomain::Corner: {
          bke::attribute_math::gather_group_to_group(
              base_faces,
              result_faces,
              unselected_faces,
              src_span,
              dst_attr.span.take_front(unselected_corners_num));
          // TODO
          break;
        }
        default: {
          break;
        }
      }
      dst_attr.finish();
    }
  });

  return result;
}

}  // namespace blender::geometry
