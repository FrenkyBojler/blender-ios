/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_delaunay_2d.hh"

#include "BKE_mesh.hh"

#include "GEO_CDT_to_mesh.hh"

namespace blender::geometry {

using namespace meshintersect;

Mesh *cdts_to_mesh(const Span<CDT_result<double>> results)
{
  /* Converting a single CDT result to a Mesh would be simple because the indices could be re-used.
   * However, in the general case here we need to combine several CDT results into a single Mesh,
   * which requires us to map the original indices to a new set of indices.
   * In order to allow for parallelization when appropriate, this implementation starts by
   * determining (for each domain) what range of indices in the final mesh data will be used for
   * each CDT result. The index ranges are represented as offsets, which are referred to as "group
   * offsets" to distinguish them from the other types of offsets we need to work with here.
   * Since it's likely that most invocations will only have a single CDT result, it's important
   * that case is made as optimal as feasible. */

  Array<int> vert_groups_data(results.size() + 1);
  Array<int> edge_groups_data(results.size() + 1);
  Array<int> face_groups_data(results.size() + 1);
  Array<int> loop_groups_data(results.size() + 1);
  threading::parallel_for(results.index_range(), 1024, [&](const IndexRange results_range) {
    for (const int i_result : results_range) {
      const CDT_result<double> &result = results[i_result];
      vert_groups_data[i_result] = result.vert.size();
      edge_groups_data[i_result] = result.edge.size();
      face_groups_data[i_result] = result.face.size();
      int loop_len = 0;
      for (const Vector<int> &face : result.face) {
        loop_len += face.size();
      }
      loop_groups_data[i_result] = loop_len;
    }
  });

  const OffsetIndices vert_groups = offset_indices::accumulate_counts_to_offsets(vert_groups_data);
  const OffsetIndices edge_groups = offset_indices::accumulate_counts_to_offsets(edge_groups_data);
  const OffsetIndices face_groups = offset_indices::accumulate_counts_to_offsets(face_groups_data);
  const OffsetIndices loop_groups = offset_indices::accumulate_counts_to_offsets(loop_groups_data);

  Mesh *mesh = BKE_mesh_new_nomain(vert_groups.total_size(),
                                   edge_groups.total_size(),
                                   face_groups.total_size(),
                                   loop_groups.total_size());

  MutableSpan<float3> all_positions = mesh->vert_positions_for_write();
  MutableSpan<int2> all_edges = mesh->edges_for_write();
  MutableSpan<int> all_face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> all_corner_verts = mesh->corner_verts_for_write();

  threading::parallel_for(results.index_range(), 1024, [&](const IndexRange results_range) {
    for (const int i_result : results_range) {
      const CDT_result<double> &result = results[i_result];
      const IndexRange verts_range = vert_groups[i_result];
      const IndexRange edges_range = edge_groups[i_result];
      const IndexRange faces_range = face_groups[i_result];
      const IndexRange loops_range = loop_groups[i_result];

      MutableSpan<float3> positions = all_positions.slice(verts_range);
      for (const int i : result.vert.index_range()) {
        positions[i] = float3(float(result.vert[i].x), float(result.vert[i].y), 0.0f);
      }

      MutableSpan<int2> edges = all_edges.slice(edges_range);
      for (const int i : result.edge.index_range()) {
        edges[i] = int2(result.edge[i].first + verts_range.start(),
                        result.edge[i].second + verts_range.start());
      }

      MutableSpan<int> face_offsets = all_face_offsets.slice(faces_range);
      MutableSpan<int> corner_verts = all_corner_verts.slice(loops_range);
      int i_face_corner = 0;
      for (const int i_face : result.face.index_range()) {
        face_offsets[i_face] = i_face_corner + loops_range.start();
        for (const int i_corner : result.face[i_face].index_range()) {
          corner_verts[i_face_corner] = result.face[i_face][i_corner] + verts_range.start();
          i_face_corner++;
        }
      }
    }
  });

  /* The delaunay triangulation doesn't seem to return all of the necessary all_edges, even in
   * triangulation mode. */
  bke::mesh_calc_edges(*mesh, true, false);
  bke::mesh_smooth_set(*mesh, false);

  mesh->tag_overlapping_none();

  return mesh;
}

}  // namespace blender::geometry
