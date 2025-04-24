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

struct EdgeOneCorner {
  int corner;
  bool winding_torwards_vert;
};

struct EdgeTwoCorners {
  int corner1;
  int corner2;
};

struct EdgeSharp {};

using VertEdgeInfo = std::variant<std::monostate, EdgeOneCorner, EdgeTwoCorners, EdgeSharp>;

static VertEdgeInfo add_corner_to_edge(const Span<int> corner_edges,
                                       const Span<bool> sharp_edges,
                                       const int corner,
                                       const int other_corner,
                                       const bool winding_torwards_vert,
                                       const VertEdgeInfo &info)
{
  if (std::holds_alternative<EdgeSharp>(info)) {
    return EdgeSharp{};
  }
  if (std::holds_alternative<std::monostate>(info)) {
    if (!sharp_edges.is_empty()) {
      /* The first time we encounter the edge, we check if it is marked sharp. In that case corner
       * fans shouldn't propagate past it. To find the edge we need to check if the current corner
       * references the edge connected to `other_corner` or if `other_corner` uses the edge. */
      if (sharp_edges[corner_edges[winding_torwards_vert ? other_corner : corner]]) {
        return EdgeSharp{};
      }
    }
    return EdgeOneCorner{corner, winding_torwards_vert};
  }
  if (const EdgeOneCorner *info_one_edge = std::get_if<EdgeOneCorner>(&info)) {
    /* If the edge ends up being used by faces, we still have to check if the winding direction
     * changes. Though it's an undesireable situation for the mesh to be in, we shouldn't propogate
     * smooth normals across edges facing opposite directions.*/
    if (info_one_edge->winding_torwards_vert && winding_torwards_vert) {
      return EdgeSharp{};
    }
    return EdgeTwoCorners{info_one_edge->corner, other_corner};
  }
  if (std::holds_alternative<EdgeTwoCorners>(info)) {
    /* The edge is already used by two corners. Adding a third would make it non-manifold,
     * which means it should be considered sharp for the purposes of normal computation. */
    return EdgeSharp{};
  }
  BLI_assert_unreachable();
  return EdgeSharp{};
}

static void add_corner_to_edge(const Span<int> corner_verts,
                               const Span<int> corner_edges,
                               const Span<bool> sharp_edges,
                               const int corner,
                               const int other_corner,
                               const bool winding_torwards_vert,
                               Map<int, VertEdgeInfo, 16> &vert_edge_infos)
{
  VertEdgeInfo &info = vert_edge_infos.lookup_or_add_default(corner_verts[other_corner]);
  info = add_corner_to_edge(
      corner_edges, sharp_edges, corner, other_corner, winding_torwards_vert, info);
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
      }

      for (const int i : vert_faces.index_range()) {
        const int face = vert_faces[i];
        if (!sharp_faces.is_empty() && sharp_faces[face]) {
          continue;
        }
        add_corner_to_edge(corner_verts,
                           corner_edges,
                           sharp_edges,
                           corner_infos[i].corner,
                           corner_infos[i].corner_prev,
                           true,
                           vert_edge_infos);
        add_corner_to_edge(corner_verts,
                           corner_edges,
                           sharp_edges,
                           corner_infos[i].corner,
                           corner_infos[i].corner_next,
                           false,
                           vert_edge_infos);
      }
    }
  });
}

}  // namespace blender::bke::mesh

/** \} */
