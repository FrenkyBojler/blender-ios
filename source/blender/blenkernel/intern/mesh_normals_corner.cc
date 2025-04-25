/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// #define DEBUG_TIME

#include "BKE_mesh.hh"

#include "BLI_disjoint_set.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_vector_set.hh"

#ifdef DEBUG_TIME
#  include "BLI_timeit.hh"
#endif

namespace blender::bke::mesh {

struct VertCornerInfo {
  int face;
  int corner;
  int corner_prev;
  int corner_next;
  int vert_prev;
  int vert_next;
};

static void collect_corner_info(const OffsetIndices<int> faces,
                                const Span<int> corner_verts,
                                const Span<int> vert_faces,
                                const int vert,
                                MutableSpan<VertCornerInfo> r_corner_infos)
{
  for (const int i : vert_faces.index_range()) {
    const int face = vert_faces[i];
    r_corner_infos[i].face = face;
    r_corner_infos[i].corner = face_find_corner_from_vert(faces[face], corner_verts, vert);
    r_corner_infos[i].corner_prev = face_corner_prev(faces[face], r_corner_infos[i].corner);
    r_corner_infos[i].corner_next = face_corner_next(faces[face], r_corner_infos[i].corner);
    r_corner_infos[i].vert_prev = corner_verts[r_corner_infos[i].corner_prev];
    r_corner_infos[i].vert_next = corner_verts[r_corner_infos[i].corner_next];
  }
}

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

static void calc_local_edge_indices(const Span<VertCornerInfo> corner_infos,
                                    VectorSet<int> &r_other_vert_to_edge)
{
  for (const VertCornerInfo &info : corner_infos) {
    r_other_vert_to_edge.add(info.vert_prev);
    r_other_vert_to_edge.add(info.vert_next);
  }
}

static void calc_connecting_edge_info(const Span<int> corner_edges,
                                      const Span<bool> sharp_edges,
                                      const Span<bool> sharp_faces,
                                      const Span<VertCornerInfo> corner_infos,
                                      const VectorSet<int> &other_vert_edge_indices,
                                      MutableSpan<VertEdgeInfo> vert_edge_infos)
{
  vert_edge_infos.fill(std::monostate{});
  for (const int i : corner_infos.index_range()) {
    const VertCornerInfo &info = corner_infos[i];
    const int face = info.face;
    const int edge_prev = other_vert_edge_indices.index_of(info.vert_prev);
    const int edge_next = other_vert_edge_indices.index_of(info.vert_next);
    if (!sharp_faces.is_empty() && sharp_faces[face]) {
      vert_edge_infos[edge_prev] = EdgeSharp{};
      vert_edge_infos[edge_next] = EdgeSharp{};
      continue;
    }
    vert_edge_infos[edge_prev] = add_corner_to_edge(corner_edges,
                                                    sharp_edges,
                                                    info.corner,
                                                    info.corner_prev,
                                                    true,
                                                    vert_edge_infos[edge_prev]);
    vert_edge_infos[edge_next] = add_corner_to_edge(corner_edges,
                                                    sharp_edges,
                                                    info.corner,
                                                    info.corner_next,
                                                    false,
                                                    vert_edge_infos[edge_next]);
  }
}

static float3 calc_smooth_vert_normal(const Span<float3> positions,
                                      const Span<VertCornerInfo> corner_infos,
                                      const Span<int> corner_verts,
                                      const int vert,
                                      const Span<float3> face_normals)
{
  float3 vert_normal(0);
  for (const int i : corner_infos.index_range()) {
    const int vert_prev = corner_verts[corner_infos[i].corner_prev];
    const int vert_next = corner_verts[corner_infos[i].corner_next];
    const float3 dir_prev = math::normalize(positions[vert_prev] - positions[vert]);
    const float3 dir_next = math::normalize(positions[vert_next] - positions[vert]);
    const float factor = math::safe_acos_approx(math::dot(dir_prev, dir_next));

    vert_normal += face_normals[corner_infos[i].face] * factor;
  }
  return math::normalize(vert_normal);
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
    VectorSet<int> other_vert_edge_indices;  // TODO: Inline buffer size
    Vector<VertEdgeInfo, 16> edge_infos;
    Vector<bool, 16> corner_used;
    Vector<float3, 16> edge_dirs;
    Vector<int, 16> corners_in_fan;
    // DisjointSet<int> disjoint_set;
    for (const int vert : range) {
      const float3 vert_position = vert_positions[vert];
      const Span<int> vert_faces = vert_to_face_map[vert];

      if (vert_faces.is_empty()) {
        r_corner_normals[vert] = math::normalize(vert_position);
        continue;
      }

      corner_infos.resize(vert_faces.size());
      collect_corner_info(faces, corner_verts, vert_faces, vert, corner_infos);

      other_vert_edge_indices.clear();
      calc_local_edge_indices(corner_infos, other_vert_edge_indices);

      calc_connecting_edge_info(corner_edges,
                                sharp_edges,
                                sharp_faces,
                                corner_infos,
                                other_vert_edge_indices,
                                edge_infos);

      const int sharp_edges_num = std::count_if(
          edge_infos.begin(), edge_infos.end(), [](const auto &info) {
            return std::holds_alternative<EdgeSharp>(info);
          });

      if (sharp_edges_num == 0) {
        r_corner_normals[vert] = calc_smooth_vert_normal(
            vert_positions, corner_infos, corner_verts, vert, face_normals);
        continue;
      }

      if (sharp_edges_num == edge_infos.size()) {
        for (const VertCornerInfo &info : corner_infos) {
          r_corner_normals[info.corner] = face_normals[info.face];
        }
        continue;
      }

      edge_dirs.resize(vert_faces.size());
      for (const int i : other_vert_edge_indices.index_range()) {
        edge_dirs[i] = math::normalize(vert_positions[other_vert_edge_indices[i]] - vert_position);
      }

      corner_used.resize(vert_faces.size());
      corner_used.fill(false);

      for (int i = 0; i != -1; i = corner_used.first_index_of_try(false)) {
        corner_used[i] = true;

        const VertCornerInfo &info = corner_infos[i];
        const int corner = info.corner;

        const int vert_prev = corner_verts[info.corner_prev];
        const int vert_next = corner_verts[info.corner_next];
        const int edge_prev = other_vert_edge_indices.index_of(vert_prev);
        const int edge_next = other_vert_edge_indices.index_of(vert_next);

        const VertEdgeInfo &edge_info_prev = edge_infos[edge_prev];
        const VertEdgeInfo &edge_info_next = edge_infos[edge_next];

        if (std::holds_alternative<EdgeSharp>(edge_info_prev) &&
            std::holds_alternative<EdgeSharp>(edge_info_next))
        {
          r_corner_normals[corner] = face_normals[info.face];
          continue;
        }

        float3 normal(0);

        const float factor = math::safe_acos_approx(math::dot(dir_prev, dir_next));
        normal += face_normals[info.face] * factor;

        /* Travel along the previous edge.*/
        while (false) {
        }

        /* Travel along the next edge.*/
        while (false) {
        }

        corners_in_fan.append(corner);
      }
    }
  });
}

}  // namespace blender::bke::mesh

/** \} */
