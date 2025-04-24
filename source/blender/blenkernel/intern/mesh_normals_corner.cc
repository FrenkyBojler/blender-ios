/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// #define DEBUG_TIME

#include "BKE_mesh.hh"

#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"

#ifdef DEBUG_TIME
#  include "BLI_timeit.hh"
#endif

namespace blender::bke::mesh {

struct VertCornerInfo {
  int face;
  int corner;
  int corner_next;
  int corner_prev;
};

struct VertEdgeInfo {
  Vector<int> corners;
  bool connection;
};

static void add_corner_to_edge()
{
  // Edge is not a connection if it's a sharp edge
  // Edge is not a connection if winding is reversed compared to previously added face
  // Edge is not a connection if it's used by more than two faces
}

void normals_calc_corners(const Span<float3> vert_positions,
                          const OffsetIndices<int> faces,
                          const Span<int> corner_verts,
                          const Span<int> corner_edges,
                          const GroupedSpan<int> vert_to_face_map,
                          const Span<float3> face_normals,
                          const Span<bool> sharp_edges,
                          const Span<bool> sharp_faces,
                          const Span<short2> custom_normals,
                          CornerNormalSpaceArray *r_lnors_spacearr,
                          MutableSpan<float3> r_corner_normals)
{
  // for vert : verts:
  //     corners = get_corners(faces, vert_to_face_map)
  //     sort_corners(corners)
  //     calc

  threading::parallel_for(vert_positions.index_range(), 512, [&](const IndexRange range) {
    Vector<VertCornerInfo, 16> corner_infos;
    Map<int, VertEdgeInfo, 16> vert_edge_infos;
    for (const int vert : range) {
      const Span<int> vert_faces = vert_to_face_map[vert];
      if (vert_faces.is_empty()) {
        r_corner_normals[vert] = math::normalize(vert_positions[vert]);
        continue;
      }
      corner_infos.resize(vert_faces.size());
      vert_edge_infos.clear_and_keep_capacity();
      for (const int i : vert_faces.index_range()) {
        const int face = vert_faces[i];
        corner_infos[i].face = face;
        corner_infos[i].corner = face_find_corner_from_vert(faces[face], corner_verts, vert);
        corner_infos[i].corner_prev = face_corner_prev(faces[face], corner_infos[i].corner);
        corner_infos[i].corner_next = face_corner_next(faces[face], corner_infos[i].corner);

        const int other_vert_prev = corner_verts[corner_infos[i].corner_prev];
        const int other_vert_next = corner_verts[corner_infos[i].corner_next];
        // vert_edge_infos.

        VertEdgeInfo &prev_edge = vert_edge_infos.lookup_or_add_default(other_vert_prev);
        prev_edge.corners.append(corner_infos[i].corner);
        prev_edge.connection = true;  // TODO

        VertEdgeInfo &next_edge = vert_edge_infos.lookup_or_add_default(other_vert_next);
        next_edge.corners.append(corner_infos[i].corner);
        next_edge.connection = true;  // TODO
      }

      // sort corners by topological connectivity
    }
  });
}

}  // namespace blender::bke::mesh

/** \} */
