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

/**
 * Gather data related to all the connected faces / face corners. This makes accessing it simpler
 * later on in the various per-vertex hot loops. It also means we can be sure it will be in CPU
 * caches. Gathering it into a single Vector of an "info" struct rather than multiple vectors is
 * expected to be worth it because there are typically very few connected corners; the overhead of
 * a Vector for each piece of data would be significant.
 */
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

/** The edge hasn't been handled yet while the edge info is being created. */
struct EdgeUninitialized {};

/**
 * The first corner has been added to the edge. For boundary edges, this is the only corner. We
 * store whether the winding direction of the face was towards or away from the vertex to be able
 * to detect when the winding direction of two neighboring faces doesn't match.
 */
struct EdgeOneCorner {
  int local_corner_1;
  bool winding_torwards_vert;
};

/**
 * The edge is manifold and is used by two faces/corners. The actual faces and corners have to be
 * retrieved with the data in #VertCornerInfo.
 */
struct EdgeTwoCorners {
  int local_corner_1;
  int local_corner_2;
};

/**
 * The edge "breaks" the topology flow of faces around the vertex. It could be marked sharp
 * explicitly, it could be used by a sharp face, it could have mismatched face winding directions,
 * or it might be non-manifold and used by more than two faces.
 */
struct EdgeSharp {};

using VertEdgeInfo = std::variant<EdgeUninitialized, EdgeOneCorner, EdgeTwoCorners, EdgeSharp>;

static void add_corner_to_edge(const Span<int> corner_edges,
                               const Span<bool> sharp_edges,
                               const int local_corner,
                               const int corner,
                               const int other_corner,
                               const bool winding_torwards_vert,
                               VertEdgeInfo &info)
{
  if (std::holds_alternative<EdgeUninitialized>(info)) {
    if (!sharp_edges.is_empty()) {
      /* The first time we encounter the edge, we check if it is marked sharp. In that case corner
       * fans shouldn't propagate past it. To find the edge we need to check if the current corner
       * references the edge connected to `other_corner` or if `other_corner` uses the edge. */
      if (sharp_edges[corner_edges[winding_torwards_vert ? other_corner : corner]]) {
        info = EdgeSharp{};
        return;
      }
    }
    info = EdgeOneCorner{local_corner, winding_torwards_vert};
  }
  else if (const EdgeOneCorner *info_one_edge = std::get_if<EdgeOneCorner>(&info)) {
    /* If the edge ends up being used by faces, we still have to check if the winding direction
     * changes. Though it's an undesireable situation for the mesh to be in, we shouldn't propogate
     * smooth normals across edges facing opposite directions.*/
    if (info_one_edge->winding_torwards_vert && winding_torwards_vert) {
      info = EdgeSharp{};
      return;
    }
    info = EdgeTwoCorners{info_one_edge->local_corner_1, local_corner};
  }
  else {
    info = EdgeSharp{};
  }
  /* The edge is either already sharp, or we're trying to add a third corner. */
}

/** Use a custom VectorSet type to use int32 instead of int64 for the key indices. */
using LocalEdgeVectorSet = VectorSet<int,
                                     DefaultProbingStrategy,
                                     DefaultHash<int>,
                                     DefaultEquality<int>,
                                     SimpleVectorSetSlot<int, int>,
                                     GuardedAllocator>;

/**
 * Create a local indexing for the edges connected to the vertex (not including loose edges of
 * course). We could look up the edge indices from the VectorSet as necessary later, but it should
 * be better to just use a bit more space in #VertCornerInfo to simplify things instead.
 */
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
  for (const int local_corner : corner_infos.index_range()) {
    const VertCornerInfo &info = corner_infos[local_corner];
    if (!sharp_faces.is_empty() && sharp_faces[info.face]) {
      /* Sharp faces implicitly cause sharp edges. */
      vert_edge_infos[info.local_edge_prev] = EdgeSharp{};
      vert_edge_infos[info.local_edge_next] = EdgeSharp{};
      continue;
    }
    /* The "previous" edge is winding towards the vertex, the "next" edge is winding away. */
    add_corner_to_edge(corner_edges,
                       sharp_edges,
                       local_corner,
                       info.corner,
                       info.corner_prev,
                       true,
                       vert_edge_infos[info.local_edge_prev]);
    add_corner_to_edge(corner_edges,
                       sharp_edges,
                       local_corner,
                       info.corner,
                       info.corner_next,
                       false,
                       vert_edge_infos[info.local_edge_next]);
  }
}

/**
 * From a starting corner, follow the connected edges to find the other corners "fanning" arount
 * the vertex. Crucially, we've removed ambiguity from the process already by marking edges
 * connected to three faces sharp.
 */
static void traverse_fan_local_corners(const Span<VertCornerInfo> corner_infos,
                                       const Span<VertEdgeInfo> edge_infos,
                                       const int start_local_corner,
                                       Vector<int, 16> &result_fan)
{
  result_fan.append(start_local_corner);

  {
    /* Travel in the "previous" direction. */
    const int start_size = result_fan.size();
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
    /* Reverse the corners added so the final order is consistent with the next traversal. */
    result_fan.as_mutable_span().drop_front(start_size).reverse();
  }

  {
    /* Travel in the "next" direction. */
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

/**
 * The edge directions are used to compute factors for the face normals from each corner. Since
 * they involve a normalization it's worth it to compute them once, especially since we've
 * deduplicated the edge indices and can easily index them with #VertCornerInfo.
 */
static void calc_edge_directions(const Span<float3> vert_positions,
                                 const Span<int> local_edge_by_vert,
                                 const float3 &vert_position,
                                 MutableSpan<float3> edge_dirs)
{
  for (const int i : local_edge_by_vert.index_range()) {
    edge_dirs[i] = math::normalize(vert_positions[local_edge_by_vert[i]] - vert_position);
  }
}

/**
 * This is the same as #normals_calc_vert, but uses our already-collected corner info and edge
 * directions. This case where all the edges aren't smooth is very common and likely worth handling
 * explicitly.
 */
static float3 calc_smooth_vert_normal(const Span<VertCornerInfo> corner_infos,
                                      const Span<float3> edge_dirs,
                                      const Span<float3> face_normals)
{
  float3 vert_normal(0);
  for (const int i : corner_infos.index_range()) {
    const VertCornerInfo &info = corner_infos[i];
    const float3 &dir_prev = edge_dirs[info.local_edge_prev];
    const float3 &dir_next = edge_dirs[info.local_edge_next];
    const float factor = math::safe_acos_approx(math::dot(dir_prev, dir_next));
    vert_normal += face_normals[info.face] * factor;
  }
  return math::normalize(vert_normal);
}

/** The normal for all the corners in the fan is a weighted combination of their face normals. */
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

      /* Because we're iterating over vertices in order to batch work for their connected face
       * corners, we have to handle loose vertices and vertices not used by faces. */
      if (vert_faces.is_empty()) {
        r_corner_normals[vert] = math::normalize(vert_position);
        continue;
      }

      corner_infos.resize(vert_faces.size());
      collect_corner_info(faces, corner_verts, vert_faces, vert, corner_infos);

      local_edge_by_vert.clear_and_keep_capacity();
      calc_local_edge_indices(corner_infos, local_edge_by_vert);

      edge_infos.clear();
      edge_infos.resize(corner_infos.size());
      calc_connecting_edge_info(corner_edges, sharp_edges, sharp_faces, corner_infos, edge_infos);

      const int sharp_edges_num = std::count_if(
          edge_infos.begin(), edge_infos.end(), [](const auto &info) {
            return std::holds_alternative<EdgeSharp>(info);
          });

      /* Check whether all faces are sharp. This situation might be common on meshes that are
       * mostly sharp shaded, and just copying the face normals is so much faster that it's likely
       * worth handling it explicitly. */
      if (sharp_edges_num == edge_infos.size()) {
        for (const VertCornerInfo &info : corner_infos) {
          r_corner_normals[info.corner] = face_normals[info.face];
        }
        continue;
      }

      edge_dirs.resize(vert_faces.size());
      calc_edge_directions(vert_positions, local_edge_by_vert, vert_position, edge_dirs);

      if (sharp_edges_num == 0) {
        const float3 normal = calc_smooth_vert_normal(corner_infos, edge_dirs, face_normals);
        for (const VertCornerInfo &info : corner_infos) {
          r_corner_normals[info.corner] = normal;
        }
        continue;
      }

      /* Though we are protected from traversing to the same corner twice by the fact that 3-way
       * connections are marked sharp, we need to maintain the "visited" status of each corner so
       * we can find the next start corner for each subsequent fan traversal. Keeping track of the
       * number of visited corners is a quick way to avoid this book keeping for the final fan (and
       * there are usually just two, so that should be worth it). */
      int visited_corners = 0;
      local_corner_visited.resize(vert_faces.size());
      local_corner_visited.fill(false);

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
