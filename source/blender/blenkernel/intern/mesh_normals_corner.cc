/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// #define DEBUG_TIME

#include "BKE_mesh.hh"

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
  int local_edge_prev;
  int local_edge_next;
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
  int local_corner_1;
  bool winding_torwards_vert;
};

struct EdgeTwoCorners {
  int local_corner_1;
  int local_corner_2;
};

struct EdgeSharp {};

using VertEdgeInfo = std::variant<std::monostate, EdgeOneCorner, EdgeTwoCorners, EdgeSharp>;

static VertEdgeInfo add_corner_to_edge(const Span<int> corner_edges,
                                       const Span<bool> sharp_edges,
                                       const int local_corner,
                                       const int corner,
                                       const int other_corner,
                                       const bool winding_torwards_vert,
                                       const VertEdgeInfo &info)
{
  if (std::holds_alternative<std::monostate>(info)) {
    if (!sharp_edges.is_empty()) {
      /* The first time we encounter the edge, we check if it is marked sharp. In that case corner
       * fans shouldn't propagate past it. To find the edge we need to check if the current corner
       * references the edge connected to `other_corner` or if `other_corner` uses the edge. */
      if (sharp_edges[corner_edges[winding_torwards_vert ? other_corner : corner]]) {
        return EdgeSharp{};
      }
    }
    return EdgeOneCorner{local_corner, winding_torwards_vert};
  }
  if (const EdgeOneCorner *info_one_edge = std::get_if<EdgeOneCorner>(&info)) {
    /* If the edge ends up being used by faces, we still have to check if the winding direction
     * changes. Though it's an undesireable situation for the mesh to be in, we shouldn't propogate
     * smooth normals across edges facing opposite directions.*/
    if (info_one_edge->winding_torwards_vert && winding_torwards_vert) {
      return EdgeSharp{};
    }
    return EdgeTwoCorners{info_one_edge->local_corner_1, local_corner};
  }
  if (std::holds_alternative<EdgeSharp>(info)) {
    return EdgeSharp{};
  }
  if (std::holds_alternative<EdgeTwoCorners>(info)) {
    /* The edge is already used by two corners. Adding a third would make it non-manifold,
     * which means it should be considered sharp for the purposes of normal computation. */
    return EdgeSharp{};
  }
  /* This case should not happen. */
  return EdgeSharp{};
}

using LocalEdgeVectorSet = VectorSet<int,
                                     DefaultProbingStrategy,
                                     DefaultHash<int>,
                                     DefaultEquality<int>,
                                     SimpleVectorSetSlot<int, int>,
                                     GuardedAllocator>;

static void calc_local_edge_indices(MutableSpan<VertCornerInfo> corner_infos,
                                    LocalEdgeVectorSet &r_other_vert_to_edge)
{
  r_other_vert_to_edge.reserve(corner_infos.size());
  for (VertCornerInfo &info : corner_infos) {
    info.local_edge_prev = r_other_vert_to_edge.index_of_or_add(info.vert_prev);
    info.local_edge_next = r_other_vert_to_edge.index_of_or_add(info.vert_next);
  }
}

static void calc_connecting_edge_info(const Span<int> corner_edges,
                                      const Span<bool> sharp_edges,
                                      const Span<bool> sharp_faces,
                                      const Span<VertCornerInfo> corner_infos,
                                      MutableSpan<VertEdgeInfo> vert_edge_infos)
{
  vert_edge_infos.fill(std::monostate{});
  for (const int local_corner : corner_infos.index_range()) {
    const VertCornerInfo &info = corner_infos[local_corner];
    const int face = info.face;
    const int edge_prev = info.local_edge_prev;
    const int edge_next = info.local_edge_next;
    if (!sharp_faces.is_empty() && sharp_faces[face]) {
      vert_edge_infos[edge_prev] = EdgeSharp{};
      vert_edge_infos[edge_next] = EdgeSharp{};
      continue;
    }
    vert_edge_infos[edge_prev] = add_corner_to_edge(corner_edges,
                                                    sharp_edges,
                                                    local_corner,
                                                    info.corner,
                                                    info.corner_prev,
                                                    true,
                                                    vert_edge_infos[edge_prev]);
    vert_edge_infos[edge_next] = add_corner_to_edge(corner_edges,
                                                    sharp_edges,
                                                    local_corner,
                                                    info.corner,
                                                    info.corner_next,
                                                    false,
                                                    vert_edge_infos[edge_next]);
  }
}

static float3 calc_smooth_vert_normal(const Span<float3> positions,
                                      const Span<VertCornerInfo> corner_infos,
                                      const int vert,
                                      const Span<float3> face_normals)
{
  float3 vert_normal(0);
  for (const int i : corner_infos.index_range()) {
    const VertCornerInfo &info = corner_infos[i];
    const float3 dir_prev = math::normalize(positions[info.vert_prev] - positions[vert]);
    const float3 dir_next = math::normalize(positions[info.vert_next] - positions[vert]);
    const float factor = math::safe_acos_approx(math::dot(dir_prev, dir_next));

    vert_normal += face_normals[corner_infos[i].face] * factor;
  }
  return math::normalize(vert_normal);
}

static void traverse_fan_local_corners(const Span<VertCornerInfo> corner_infos,
                                       const Span<VertEdgeInfo> edge_infos,
                                       const int start_local_corner,
                                       Vector<int, 16> &result_fan)
{
  const int start_size = result_fan.size();
  result_fan.append(start_local_corner);

  {
    int current = start_local_corner;
    int edge_prev = corner_infos[current].local_edge_prev;
    while (const EdgeTwoCorners *edge = std::get_if<EdgeTwoCorners>(&edge_infos[edge_prev])) {
      current = current == edge->local_corner_1 ? edge->local_corner_2 : edge->local_corner_1;
      if (current == start_local_corner) {
        break;
      }
      result_fan.append(current);
      edge_prev = corner_infos[current].local_edge_prev;
    }
  }

  MutableSpan<int> reverse_traversal = result_fan.as_mutable_span().drop_front(start_size);
  std::reverse(reverse_traversal.begin(), reverse_traversal.end());

  {
    int current = start_local_corner;
    int edge_next = corner_infos[current].local_edge_next;
    while (const EdgeTwoCorners *edge = std::get_if<EdgeTwoCorners>(&edge_infos[edge_next])) {
      current = current == edge->local_corner_1 ? edge->local_corner_2 : edge->local_corner_1;
      if (current == start_local_corner) {
        break;
      }
      result_fan.append(current);
      edge_next = corner_infos[current].local_edge_next;
    }
  }
}

static void calc_edge_directions(const Span<float3> vert_positions,
                                 const LocalEdgeVectorSet &local_edge_by_vert,
                                 const float3 &vert_position,
                                 MutableSpan<float3> edge_dirs)
{
  for (const int i : local_edge_by_vert.index_range()) {
    edge_dirs[i] = math::normalize(vert_positions[local_edge_by_vert[i]] - vert_position);
  }
}

static float3 accumulate_fan_normal(const Span<VertCornerInfo> corner_infos,
                                    const Span<float3> edge_dirs,
                                    const Span<float3> face_normals,
                                    const Span<int> local_corners_in_fan)
{
  float3 fan_normal(0);
  for (const int local_corner : local_corners_in_fan) {
    const VertCornerInfo &info = corner_infos[local_corner];

    const float3 &dir_prev = edge_dirs[info.local_edge_prev];
    const float3 &dir_next = edge_dirs[info.local_edge_next];

    const float factor = math::safe_acos_approx(math::dot(dir_prev, dir_next));
    fan_normal += face_normals[info.face] * factor;
  }

  return math::normalize(fan_normal);
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
  threading::parallel_for(vert_positions.index_range(), 256, [&](const IndexRange range) {
    Vector<VertCornerInfo, 16> corner_infos;
    LocalEdgeVectorSet local_edge_by_vert;  // TODO: Inline buffer size
    Vector<VertEdgeInfo, 16> edge_infos;
    Vector<float3, 16> edge_dirs;
    Vector<bool, 16> local_corner_visited;
    Vector<int, 16> corners_in_fan;
    for (const int vert : range) {
      const float3 vert_position = vert_positions[vert];
      const Span<int> vert_faces = vert_to_face_map[vert];

      if (vert_faces.is_empty()) {
        r_corner_normals[vert] = math::normalize(vert_position);
        continue;
      }

      corner_infos.resize(vert_faces.size());
      collect_corner_info(faces, corner_verts, vert_faces, vert, corner_infos);

      local_edge_by_vert.clear_and_keep_capacity();
      calc_local_edge_indices(corner_infos, local_edge_by_vert);

      edge_infos.resize(corner_infos.size());
      calc_connecting_edge_info(corner_edges, sharp_edges, sharp_faces, corner_infos, edge_infos);

      const int sharp_edges_num = std::count_if(
          edge_infos.begin(), edge_infos.end(), [](const auto &info) {
            return std::holds_alternative<EdgeSharp>(info);
          });

      // TODO: Test if this special case is actually helpful. The fully sharp case probably is,
      // though that's probably much less common in real meshes.
      if (sharp_edges_num == 0) {
        const float3 normal = calc_smooth_vert_normal(
            vert_positions, corner_infos, vert, face_normals);
        for (const VertCornerInfo &info : corner_infos) {
          r_corner_normals[info.corner] = normal;
        }
        continue;
      }

      if (sharp_edges_num == edge_infos.size()) {
        for (const VertCornerInfo &info : corner_infos) {
          r_corner_normals[info.corner] = face_normals[info.face];
        }
        continue;
      }

      edge_dirs.resize(vert_faces.size());
      calc_edge_directions(vert_positions, local_edge_by_vert, vert_position, edge_dirs);

      local_corner_visited.resize(vert_faces.size());
      local_corner_visited.fill(false);
      int visited_corners = 0;

      int start_local_corner = 0;
      while (start_local_corner != -1) {
        corners_in_fan.clear();
        traverse_fan_local_corners(corner_infos, edge_infos, start_local_corner, corners_in_fan);

        const float3 fan_normal = accumulate_fan_normal(
            corner_infos, edge_dirs, face_normals, corners_in_fan);

        for (const int local_corner : corners_in_fan) {
          const VertCornerInfo &info = corner_infos[local_corner];
          r_corner_normals[info.corner] = fan_normal;
        }

        visited_corners += corners_in_fan.size();
        if (visited_corners == corner_infos.size()) {
          break;
        }
        local_corner_visited.as_mutable_span().fill_indices(corners_in_fan.as_span(), true);
        start_local_corner = local_corner_visited.first_index_of_try(false);
      }
      BLI_assert(visited_corners == corner_infos.size());
    }
  });
}

}  // namespace blender::bke::mesh

/** \} */
