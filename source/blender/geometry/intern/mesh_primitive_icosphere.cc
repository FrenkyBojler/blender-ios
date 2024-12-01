/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <array>
#include <iostream>

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.h"
#include "BKE_mesh.hh"

#include "BLI_array_utils.hh"
#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_base.hh"
#include "BLI_math_euler.hh"
#include "BLI_math_numbers.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_rotation.hh"
#include "BLI_math_rotation_types.hh"
#include "BLI_ordered_edge.hh"
#include "BLI_span.hh"

#include "GEO_randomize.hh"

namespace blender::geometry {

static constexpr int base_verts_num = 12;
static constexpr int latitude_verts_num = (base_verts_num - 2) / 2;
static constexpr int base_edges_num = 30;
static constexpr int base_faces_num = 20;
static constexpr int base_face_quads = base_faces_num / 2;

static constexpr int face_size = 3;
static constexpr int base_face_corners_num = face_size;

namespace FaceEdge {
static constexpr int AB = 0;
static constexpr int BC = 1;
static constexpr int CA = 2;
}  // namespace FaceEdge

namespace FaceVert {
static constexpr int A = 0;
static constexpr int B = 1;
static constexpr int C = 2;
}  // namespace FaceVert

namespace EdgeVert {
static constexpr int A = 0;
static constexpr int B = 1;
}  // namespace EdgeVert

namespace Corner {
static constexpr int A = 0;
static constexpr int B = 1;
static constexpr int C = 2;
}  // namespace Corner

namespace InnerEdges {
/**
 * Base edges of face:  A-----B
 * Left edges of face:  A\\\\\B
 * Right edges of face: A/////B
 */
static constexpr int Base = 0;
static constexpr int Left = 1;
static constexpr int Right = 2;
}  // namespace InnerEdges

/**
 * Representation of triangle prefix sum. For /p base, sizes to compute prefix sum would looks
 * like: {base, base - 1, base - 2, base - 3, base - 4, ..., 0}. This triangle is sliced by
 * horizontal ranges with size = base - i. Such ranges is known as levels or floors. Count of
 * levels is equal to length of side and equal to size of base side exactly. This is equilateral
 * triangle. Total size of sum of all levels is total size of triangle. All parametrs of triangle
 * can be known in O(1) and without random memory access.
 * See: https://en.wikipedia.org/wiki/Triangular_number
 */
class TriangleRange {
 private:
  int base_;
  int total_;

 public:
  TriangleRange(const int base) : base_(base), total_(this->pyramid_sum(base))
  {
    BLI_assert(base >= 0);
  }

  IndexRange slice_at(const int level_i) const
  {
    BLI_assert(this->is_level_i(level_i));
    const int begin = this->pyramid_sum(base_ - level_i);
    const int size = base_ - level_i;
    return IndexRange(total_ - begin, size);
  }

  int start_of(const int level_i) const
  {
    BLI_assert(this->is_level_i(level_i));
    const int begin = this->pyramid_sum(base_ - level_i);
    return total_ - begin;
  }

  int end_of(const int level_i) const
  {
    BLI_assert(this->is_level_i(level_i));
    const int end = this->pyramid_sum(base_ - 1 - level_i);
    return total_ - end;
  }

  int first_of(const int level_i) const
  {
    BLI_assert(!this->slice_at(level_i).is_empty());
    const int begin = this->pyramid_sum(base_ - level_i);
    return total_ - begin;
  }

  int last_of(const int level_i) const
  {
    BLI_assert(!this->slice_at(level_i).is_empty());
    const int end = this->pyramid_sum(base_ - 1 - level_i);
    return total_ - end - 1;
  }

  int size_of(const int level_i) const
  {
    BLI_assert(this->is_level_i(level_i));
    return base_ - level_i;
  }

  int total() const
  {
    return total_;
  }

  int hight() const
  {
    return base_;
  }

  TriangleRange drop_bottom(const int n) const
  {
    return TriangleRange(math::max<int>(0, base_ - n));
  }

 private:
  const bool is_level_i(const int i) const
  {
    /* Unlike to usually reachable indices in [0, size) range, levels enumeration is in rage [0,
     * size]. So, latest element can be empty. */
    return 0 <= i && i <= base_;
  }

  static constexpr int pyramid_sum(const int i)
  {
    /* Zero level have zero length which one is known from prefix sum from -1 level... */
    BLI_assert(i >= -1);
    return i * (i + 1) / 2;
  }
};

static Span<float3> base_ico_sphere_positions()
{
  // SCOPED_TIMER_AVERAGED(__func__);
  static const auto positions = []() -> std::array<float3, base_verts_num> {
    std::array<float3, base_verts_num> positions;
    MutableSpan<float3> positions_span(positions);
    positions_span.first() = float3(0.0f, 0.0f, 1.0f);

    const float latitude = math::atan(0.5f);

    MutableSpan<float3> top_latitude = positions_span.drop_front(1).take_front(latitude_verts_num);
    MutableSpan<float3> bottom_latitude = positions_span.drop_back(1).take_back(
        latitude_verts_num);

    const constexpr float step = math::numbers::pi / latitude_verts_num * 2.0f;
    const float3 base_vector(1.0f, 0.0f, 0.0f);
    for (const int i : IndexRange(latitude_verts_num)) {
      const math::EulerXYZ rotation(0.0f, -latitude, i * step);
      top_latitude[i] = math::transform_point(math::to_quaternion(rotation), base_vector);
    }
    for (const int i : IndexRange(latitude_verts_num)) {
      const math::EulerXYZ rotation(0.0f, latitude, (i - 0.5f) * step);
      bottom_latitude[i] = math::transform_point(math::to_quaternion(rotation), base_vector);
    }

    positions_span.last() = float3(0.0f, 0.0f, -1.0f);

    BLI_assert(std::all_of(positions_span.begin(),
                           positions_span.end(),
                           [](const float3 &pos) -> bool { return math::is_unit_scale(pos); }));
    return positions;
  }();
  return positions;
}

static Span<int2> base_edge_verts_indices()
{
  // SCOPED_TIMER_AVERAGED(__func__);
  static const auto edge_verts = []() -> std::array<int2, base_edges_num> {
    std::array<int2, base_edges_num> edge_verts;

    const constexpr int first_vert = 0;
    const constexpr IndexRange top_latitude_verts(1, latitude_verts_num);
    const constexpr IndexRange bottom_latitude_verts(1 + latitude_verts_num, latitude_verts_num);
    const constexpr int last_vert = base_verts_num - 1;

    for (const int i : IndexRange(latitude_verts_num)) {
      edge_verts[latitude_verts_num * 0 + i] = int2(first_vert, top_latitude_verts[i]);
      edge_verts[latitude_verts_num * 1 + i] = int2(last_vert, bottom_latitude_verts[i]);

      const int next_i = math::mod(i + 1, latitude_verts_num);
      edge_verts[latitude_verts_num * 2 + i] = int2(top_latitude_verts[i],
                                                    top_latitude_verts[next_i]);
      edge_verts[latitude_verts_num * 3 + i] = int2(bottom_latitude_verts[i],
                                                    bottom_latitude_verts[next_i]);

      edge_verts[latitude_verts_num * 4 + i] = int2(top_latitude_verts[i],
                                                    bottom_latitude_verts[i]);
      edge_verts[latitude_verts_num * 5 + i] = int2(top_latitude_verts[i],
                                                    bottom_latitude_verts[next_i]);
    }

    return edge_verts;
  }();

  return edge_verts;
}

static Span<int3> base_face_verts_indices()
{
  // SCOPED_TIMER_AVERAGED(__func__);
  static const auto face_verts = []() -> std::array<int3, base_faces_num> {
    std::array<int3, base_faces_num> face_verts;

    const constexpr int first_vert = 0;
    const constexpr IndexRange top_latitude_verts(1, latitude_verts_num);
    const constexpr IndexRange bottom_latitude_verts(1 + latitude_verts_num, latitude_verts_num);
    const constexpr int last_vert = base_verts_num - 1;

    for (const int i : IndexRange(latitude_verts_num)) {
      const int next_i = math::mod(i + 1, latitude_verts_num);
      face_verts[latitude_verts_num * 0 + i] = int3(
          top_latitude_verts[i], top_latitude_verts[next_i], first_vert);
      face_verts[latitude_verts_num * 1 + i] = int3(
          bottom_latitude_verts[i], last_vert, bottom_latitude_verts[next_i]);
      face_verts[latitude_verts_num * 2 + i] = int3(
          top_latitude_verts[i], bottom_latitude_verts[next_i], top_latitude_verts[next_i]);
      face_verts[latitude_verts_num * 3 + i] = int3(
          bottom_latitude_verts[i], bottom_latitude_verts[next_i], top_latitude_verts[i]);
    }

    return face_verts;
  }();

  return face_verts;
}

static Span<int3> base_face_edge_indices()
{
  // SCOPED_TIMER_AVERAGED(__func__);
  static const auto face_edges = []() -> std::array<int3, base_faces_num> {
    std::array<int3, base_faces_num> face_edges;

    Span<int2> edge_verts = base_edge_verts_indices();
    Span<int3> face_verts = base_face_verts_indices();

    Map<OrderedEdge, int> edge_index_by_verts;
    for (const int i : IndexRange(base_edges_num)) {
      edge_index_by_verts.add(edge_verts[i], i);
    }

    for (const int face_i : IndexRange(base_faces_num)) {
      const OrderedEdge ab_edge_verts(face_verts[face_i][FaceVert::A],
                                      face_verts[face_i][FaceVert::B]);
      const OrderedEdge bc_edge_verts(face_verts[face_i][FaceVert::B],
                                      face_verts[face_i][FaceVert::C]);
      const OrderedEdge ca_edge_verts(face_verts[face_i][FaceVert::C],
                                      face_verts[face_i][FaceVert::A]);

      face_edges[face_i][FaceEdge::AB] = edge_index_by_verts.lookup(ab_edge_verts);
      face_edges[face_i][FaceEdge::BC] = edge_index_by_verts.lookup(bc_edge_verts);
      face_edges[face_i][FaceEdge::CA] = edge_index_by_verts.lookup(ca_edge_verts);
    }

    return face_edges;
  }();

  return face_edges;
}

static Span<float2> base_face_uv_positions()
{
  static const auto base_uv = []() -> std::array<float2, base_faces_num * face_size> {
    std::array<float2, base_faces_num * face_size> base_uv;

    static constexpr int bottom_line = 0;
    static constexpr int bottom_latitude_line = 1;
    static constexpr int top_latitude_line = 2;
    static constexpr int top_line = 3;

    static const float face_hight = math::sin(math::numbers::pi / 3.0f);

    for (const int i : IndexRange(latitude_verts_num)) {
      const float half_next_i = float(i) + 0.5f;
      const float next_i = float(i) + 1.0f;
      const int face_i = math::mod(i + 3, latitude_verts_num);
      const int face_index = (latitude_verts_num * 0 + face_i) * face_size;
      base_uv[face_index + Corner::B] = float2(next_i, top_latitude_line);
      base_uv[face_index + Corner::C] = float2(half_next_i, top_line);
      base_uv[face_index + Corner::A] = float2(i, top_latitude_line);
    }

    for (const int i : IndexRange(latitude_verts_num)) {
      const float index = i + 0.5f;
      const float half_next_i = index + 0.5f;
      const float next_i = index + 1.0f;
      const int face_i = math::mod(i + 4, latitude_verts_num);
      const int face_index = (latitude_verts_num * 1 + face_i) * face_size;
      base_uv[face_index + Corner::B] = float2(half_next_i, bottom_line);
      base_uv[face_index + Corner::C] = float2(next_i, bottom_latitude_line);
      base_uv[face_index + Corner::A] = float2(index, bottom_latitude_line);
    }

    for (const int i : IndexRange(latitude_verts_num)) {
      const float half_next_i = float(i) + 0.5f;
      const float next_i = float(i) + 1.0f;
      const int face_i = math::mod(i + 3, latitude_verts_num);
      const int face_index = (latitude_verts_num * 2 + face_i) * face_size;
      base_uv[face_index + Corner::B] = float2(half_next_i, bottom_latitude_line);
      base_uv[face_index + Corner::C] = float2(next_i, top_latitude_line);
      base_uv[face_index + Corner::A] = float2(i, top_latitude_line);
    }

    for (const int i : IndexRange(latitude_verts_num)) {
      const float index = i + 0.5f;
      const float half_next_i = index + 0.5f;
      const float next_i = index + 1.0f;
      const int face_i = math::mod(i + 4, latitude_verts_num);
      const int face_index = (latitude_verts_num * 3 + face_i) * face_size;
      base_uv[face_index + Corner::B] = float2(next_i, bottom_latitude_line);
      base_uv[face_index + Corner::C] = float2(half_next_i, top_latitude_line);
      base_uv[face_index + Corner::A] = float2(index, bottom_latitude_line);
    }

    std::transform(base_uv.begin(), base_uv.end(), base_uv.begin(), [](const float2 pos) {
      constexpr float scale_factor = 1.0f / 11.0f * 2.0f;
      return pos * scale_factor * float2(1.0f, face_hight);
    });
    return base_uv;
  }();
  return base_uv;
}

static void fill_interpolation(const float3 &begin, const float3 &end, MutableSpan<float3> verts)
{
  const float count = float(verts.size() + 1);
  for (const int i : verts.index_range()) {
    const float factor = float(i + 1) / count;
    verts[i] = bke::attribute_math::mix2<float3>(factor, begin, end);
  }
}

static void interpolate_edge_verts_linear(const int edge_verts_num, MutableSpan<float3> edge_verts)
{
  // SCOPED_TIMER_AVERAGED(__func__);
  const Span<float3> base_verts = base_ico_sphere_positions();
  const Span<int2> base_edge_verts = base_edge_verts_indices();

  const IndexRange edge_verts_range(edge_verts_num);
  for (const int edge_i : IndexRange(base_edges_num)) {
    MutableSpan<float3> verts = edge_verts.slice(edge_verts_range.step(edge_i));
    const int2 edge_vert_indices = base_edge_verts[edge_i];
    const float3 &vert_a = base_verts[edge_vert_indices[EdgeVert::A]];
    const float3 &vert_b = base_verts[edge_vert_indices[EdgeVert::B]];
    fill_interpolation(vert_a, vert_b, verts);
  }
}

static void foreach_face(const int face_verts_num, const FunctionRef<void(int)> call)
{
  const int grain_size = math::max<int>(
      1, (4096 * base_faces_num) / math::max<int>(1, face_verts_num));
  threading::parallel_for(IndexRange(base_faces_num), grain_size, [&](const IndexRange range) {
    for (const int64_t face_i : range) {
      call(face_i);
    }
  });
}

static void interpolate_face_verts_linear(const TriangleRange inner_face_verts,
                                          MutableSpan<float3> faces_verts)
{
  // SCOPED_TIMER_AVERAGED(__func__);
  const Span<float3> base_verts = base_ico_sphere_positions();
  const Span<int3> base_face_verts = base_face_verts_indices();

  const IndexRange face_verts_range(inner_face_verts.total());
  /* Bottom line of face verts plus left and right base verts is the side of triangle. */
  const float count = float(inner_face_verts.size_of(0) + 2);

  foreach_face(face_verts_range.size(), [&](const int face_i) {
    MutableSpan<float3> face_verts = faces_verts.slice(face_verts_range.step(face_i));

    const int3 face_vert_indices = base_face_verts[face_i];

    const float3 &vert_a = base_verts[face_vert_indices[FaceVert::A]];
    const float3 &vert_b = base_verts[face_vert_indices[FaceVert::B]];
    const float3 &vert_c = base_verts[face_vert_indices[FaceVert::C]];
    for (const int y_index : IndexRange(inner_face_verts.hight())) {
      MutableSpan<float3> level_verts = face_verts.slice(inner_face_verts.slice_at(y_index));

      const float factor = float(y_index + 1) / count;
      const float3 sub_a = bke::attribute_math::mix2<float3>(factor, vert_a, vert_c);
      const float3 sub_b = bke::attribute_math::mix2<float3>(factor, vert_b, vert_c);
      fill_interpolation(sub_a, sub_b, level_verts);
    }
  });
}

template<typename Func>
static void edges_line_fill_verts(const int2 ends, MutableSpan<int2> edges, Func &&func)
{
  edges.first()[EdgeVert::A] = ends[EdgeVert::A];
  for (const int i : edges.index_range().drop_back(1)) {
    const int vert_i = func(i);
    edges[i][EdgeVert::B] = vert_i;
    edges[i + 1][EdgeVert::A] = vert_i;
  }
  edges.last()[EdgeVert::B] = ends[EdgeVert::B];
}

static void vert_edge_topology(const IndexRange verts_of_edge_range,
                               const IndexRange edges_of_edge_range,
                               const IndexRange verts_of_edges_range,
                               MutableSpan<int2> edge_edges)
{
  // SCOPED_TIMER_AVERAGED(__func__);
  const Span<int2> base_edges = base_edge_verts_indices();
  if (edge_edges.size() == base_edges.size()) {
    edge_edges.copy_from(base_edges);
    return;
  }

  for (const int edge_i : IndexRange(base_edges_num)) {
    const int2 base_edge = base_edges[edge_i];
    MutableSpan<int2> edges = edge_edges.slice(edges_of_edge_range.step(edge_i));
    const IndexRange edge_verts = verts_of_edges_range.slice(verts_of_edge_range.step(edge_i));
    edges_line_fill_verts(
        base_edge, edges, [=](const int edge_i) -> int { return edge_verts[edge_i]; });
  }
}

static void face_edge_topology(const IndexRange verts_of_edge_range,
                               const IndexRange verts_of_face_range,
                               const IndexRange edges_of_face_range,
                               const IndexRange verts_of_edges_range,
                               const TriangleRange inner_face_verts,
                               const TriangleRange inner_face_edges,
                               const IndexRange faces_verts,
                               MutableSpan<int2> face_edges)
{
  // SCOPED_TIMER_AVERAGED(__func__);
  const Span<int2> base_edge_verts = base_edge_verts_indices();
  const Span<int3> base_face_verts = base_face_verts_indices();
  const Span<int3> base_faces_edges = base_face_edge_indices();

  {
    // SCOPED_TIMER_AVERAGED("face_edge_topology: 1");
    foreach_face(verts_of_face_range.size(), [&](const int face_i) {
      const int3 face_vert_indices = base_face_verts[face_i];
      const int3 face_edge_indices = base_faces_edges[face_i];
      const IndexRange face_verts = faces_verts.slice(verts_of_face_range.step(face_i));

      const IndexRange edge_ac_verts = verts_of_edges_range.slice(
          verts_of_edge_range.step(face_edge_indices[FaceEdge::CA]));
      const IndexRange edge_bc_verts = verts_of_edges_range.slice(
          verts_of_edge_range.step(face_edge_indices[FaceEdge::BC]));

      /* Check if order of vertices in face side (edge) is the same as in edge (real edge). */
      const bool edge_ac_order = int2(face_vert_indices[FaceVert::A],
                                      face_vert_indices[FaceVert::C]) ==
                                 base_edge_verts[face_edge_indices[FaceEdge::CA]];
      const bool edge_bc_order = int2(face_vert_indices[FaceVert::B],
                                      face_vert_indices[FaceVert::C]) ==
                                 base_edge_verts[face_edge_indices[FaceEdge::BC]];

      /* Edges parallel to AB base edge. */
      MutableSpan<int2> edges = face_edges.slice(
          edges_of_face_range.step(base_faces_num * InnerEdges::Base + face_i));
      for (const int line_i : IndexRange(inner_face_edges.hight())) {
        const int begin_vert = edge_ac_order ? edge_ac_verts[line_i] : edge_ac_verts.last(line_i);
        const int end_vert = edge_bc_order ? edge_bc_verts[line_i] : edge_bc_verts.last(line_i);
        const IndexRange line_verts = face_verts.slice(inner_face_verts.slice_at(line_i));
        MutableSpan<int2> line_edges = edges.slice(inner_face_edges.slice_at(line_i));
        edges_line_fill_verts(int2(begin_vert, end_vert),
                              line_edges,
                              [=](const int vert_i) -> int { return line_verts[vert_i]; });
      }
    });
  }

  {
    // SCOPED_TIMER_AVERAGED("face_edge_topology: 2");
    foreach_face(verts_of_face_range.size(), [&](const int face_i) {
      const int3 face_vert_indices = base_face_verts[face_i];
      const int3 face_edge_indices = base_faces_edges[face_i];
      const IndexRange face_verts = faces_verts.slice(verts_of_face_range.step(face_i));

      const IndexRange edge_ba_verts = verts_of_edges_range.slice(
          verts_of_edge_range.step(face_edge_indices[FaceEdge::AB]));
      const IndexRange edge_ca_verts = verts_of_edges_range.slice(
          verts_of_edge_range.step(face_edge_indices[FaceEdge::CA]));

      const bool edge_ba_order = int2(face_vert_indices[FaceVert::A],
                                      face_vert_indices[FaceVert::B]) ==
                                 base_edge_verts[face_edge_indices[FaceEdge::AB]];
      const bool edge_ca_order = int2(face_vert_indices[FaceVert::A],
                                      face_vert_indices[FaceVert::C]) ==
                                 base_edge_verts[face_edge_indices[FaceEdge::CA]];

      /* Edges parallel to BC base edge. */
      MutableSpan<int2> edges = face_edges.slice(
          edges_of_face_range.step(base_faces_num * InnerEdges::Left + face_i));
      for (const int line_i : IndexRange(inner_face_edges.hight())) {
        const int r_line_i = inner_face_edges.hight() - 1 - line_i;
        const int begin_vert = edge_ba_order ? edge_ba_verts[r_line_i] :
                                               edge_ba_verts.last(r_line_i);
        const int end_vert = edge_ca_order ? edge_ca_verts[r_line_i] :
                                             edge_ca_verts.last(r_line_i);
        MutableSpan<int2> line_edges = edges.slice(inner_face_edges.slice_at(line_i));
        edges_line_fill_verts(
            int2(begin_vert, end_vert), line_edges, [=](const int vert_i) -> int {
              return face_verts[inner_face_verts.last_of(vert_i) - line_i];
            });
      }
    });
  }

  {
    // SCOPED_TIMER_AVERAGED("face_edge_topology: 3");
    foreach_face(verts_of_face_range.size(), [&](const int face_i) {
      const int3 face_vert_indices = base_face_verts[face_i];
      const int3 face_edge_indices = base_faces_edges[face_i];
      const IndexRange face_verts = faces_verts.slice(verts_of_face_range.step(face_i));

      const IndexRange edge_ab_verts = verts_of_edges_range.slice(
          verts_of_edge_range.step(face_edge_indices[FaceEdge::AB]));
      const IndexRange edge_cb_verts = verts_of_edges_range.slice(
          verts_of_edge_range.step(face_edge_indices[FaceEdge::BC]));

      const bool edge_ab_order = int2(face_vert_indices[FaceVert::A],
                                      face_vert_indices[FaceVert::B]) ==
                                 base_edge_verts[face_edge_indices[FaceEdge::AB]];
      const bool edge_cb_order = int2(face_vert_indices[FaceVert::C],
                                      face_vert_indices[FaceVert::B]) ==
                                 base_edge_verts[face_edge_indices[FaceEdge::BC]];

      /* Edges parallel to AC base edge. */
      MutableSpan<int2> edges = face_edges.slice(
          edges_of_face_range.step(base_faces_num * InnerEdges::Right + face_i));
      for (const int line_i : IndexRange(inner_face_edges.hight())) {
        const int begin_vert = edge_ab_order ? edge_ab_verts[line_i] : edge_ab_verts.last(line_i);
        const int end_vert = edge_cb_order ? edge_cb_verts[line_i] : edge_cb_verts.last(line_i);
        MutableSpan<int2> line_edges = edges.slice(inner_face_edges.slice_at(line_i));
        edges_line_fill_verts(
            int2(begin_vert, end_vert), line_edges, [=](const int vert_i) -> int {
              return face_verts[inner_face_verts.first_of(vert_i) + line_i];
            });
      }
    });
  }
}

static bool elem_of(int elem, const int2 elems)
{
  return ELEM(elem, elems[0], elems[1]);
}

static void corner_edges_topology(const int edge_edges_num,
                                  const IndexRange edges_of_edge_range,
                                  const IndexRange faces_of_edge_range,
                                  const IndexRange faces_of_face_range,
                                  const IndexRange corner_faces_of_face_range,
                                  const IndexRange edge_faces_of_face_range,
                                  const IndexRange top_inner_faces_of_face_range,
                                  const IndexRange bottom_inner_faces_of_face_range,
                                  const TriangleRange inner_bottom_faces_of_face,
                                  const TriangleRange inner_top_faces_of_face,
                                  const TriangleRange inner_face_edges,
                                  MutableSpan<int> corner_edges)
{
  // SCOPED_TIMER_AVERAGED(__func__);
  const Span<int2> base_edge_verts = base_edge_verts_indices();
  const Span<int3> base_face_verts = base_face_verts_indices();
  const Span<int3> base_faces_edges = base_face_edge_indices();

  if (corner_edges.size() == base_faces_edges.cast<int>().size()) {
    corner_edges.copy_from(base_faces_edges.cast<int>());
    return;
  }

  foreach_face(faces_of_face_range.size(), [&](const int face_i) {
    const int3 face_vert_indices = base_face_verts[face_i];
    const int3 face_edge_indices = base_faces_edges[face_i];
    MutableSpan<int> face_corner_edges = corner_edges.slice(
        faces_of_face_range.step(face_i).scale(face_size));

    MutableSpan<int> corner_face_edges = face_corner_edges.slice(
        corner_faces_of_face_range.scale(face_size));
    MutableSpan<int> edge_face_edges = face_corner_edges.slice(
        edge_faces_of_face_range.scale(face_size));
    MutableSpan<int> top_face_edges = face_corner_edges.slice(
        top_inner_faces_of_face_range.scale(face_size));
    MutableSpan<int> bottom_face_edges = face_corner_edges.slice(
        bottom_inner_faces_of_face_range.scale(face_size));

    const IndexRange edge_a_edges = edges_of_edge_range.step(face_edge_indices[FaceEdge::AB]);
    const IndexRange edge_b_edges = edges_of_edge_range.step(face_edge_indices[FaceEdge::BC]);
    const IndexRange edge_c_edges = edges_of_edge_range.step(face_edge_indices[FaceEdge::CA]);

    /* Inner face edge ranges. */
    const IndexRange face_edges_a = IndexRange(inner_face_edges.total())
                                        .step(base_faces_num * InnerEdges::Base + face_i)
                                        .shift(base_edges_num * edge_edges_num);
    const IndexRange face_edges_b = IndexRange(inner_face_edges.total())
                                        .step(base_faces_num * InnerEdges::Left + face_i)
                                        .shift(base_edges_num * edge_edges_num);
    const IndexRange face_edges_c = IndexRange(inner_face_edges.total())
                                        .step(base_faces_num * InnerEdges::Right + face_i)
                                        .shift(base_edges_num * edge_edges_num);

    const bool corner_cab_order = elem_of(
        base_edge_verts[face_edge_indices[FaceEdge::CA]][EdgeVert::A],
        base_edge_verts[face_edge_indices[FaceEdge::BC]]);
    const bool corner_abc_order = elem_of(
        base_edge_verts[face_edge_indices[FaceEdge::AB]][EdgeVert::A],
        base_edge_verts[face_edge_indices[FaceEdge::CA]]);
    const bool corner_bca_order = elem_of(
        base_edge_verts[face_edge_indices[FaceEdge::BC]][EdgeVert::A],
        base_edge_verts[face_edge_indices[FaceEdge::AB]]);

    /* Faces in corners of base face. */
    const constexpr IndexRange single_face_range(face_size);
    MutableSpan<int> a_corner_face = corner_face_edges.slice(single_face_range.step(Corner::A));
    a_corner_face[Corner::A] = face_edges_a.last();
    a_corner_face[Corner::B] = !corner_bca_order ? edge_b_edges.first() : edge_b_edges.last();
    a_corner_face[Corner::C] = corner_cab_order ? edge_c_edges.first() : edge_c_edges.last();

    MutableSpan<int> b_corner_face = corner_face_edges.slice(single_face_range.step(Corner::B));
    b_corner_face[Corner::A] = corner_abc_order ? edge_a_edges.first() : edge_a_edges.last();
    b_corner_face[Corner::B] = face_edges_b.last();
    b_corner_face[Corner::C] = !corner_cab_order ? edge_c_edges.first() : edge_c_edges.last();

    MutableSpan<int> c_corner_face = corner_face_edges.slice(single_face_range.step(Corner::C));
    c_corner_face[Corner::A] = !corner_abc_order ? edge_a_edges.first() : edge_a_edges.last();
    c_corner_face[Corner::B] = corner_bca_order ? edge_b_edges.first() : edge_b_edges.last();
    c_corner_face[Corner::C] = face_edges_c.last();

    const bool edge_a_order = int2(face_vert_indices[FaceVert::A],
                                   face_vert_indices[FaceVert::B]) ==
                              base_edge_verts[face_edge_indices[FaceEdge::AB]];
    const bool edge_b_order = int2(face_vert_indices[FaceVert::B],
                                   face_vert_indices[FaceVert::C]) !=
                              base_edge_verts[face_edge_indices[FaceEdge::BC]];
    const bool edge_c_order = int2(face_vert_indices[FaceVert::A],
                                   face_vert_indices[FaceVert::C]) ==
                              base_edge_verts[face_edge_indices[FaceEdge::CA]];

    /* Faces along base edge. */
    for (const int i : faces_of_edge_range.index_range()) {
      const int r_i = faces_of_edge_range.last(i);
      const int inner_i = i + 1;
      const int face_i = faces_of_edge_range.step(InnerEdges::Base)[i] * face_size;
      edge_face_edges[face_i + Corner::A] = edge_a_order ? edge_a_edges[inner_i] :
                                                           edge_a_edges.last(inner_i);
      edge_face_edges[face_i + Corner::B] = face_edges_b[inner_face_edges.first_of(r_i)];
      edge_face_edges[face_i + Corner::C] = face_edges_c[inner_face_edges.first_of(i)];
    }

    for (const int i : faces_of_edge_range.index_range()) {
      const int r_i = faces_of_edge_range.last(i);
      const int inner_i = i + 1;
      const int face_i = faces_of_edge_range.step(InnerEdges::Left)[i] * face_size;
      edge_face_edges[face_i + Corner::A] = edge_b_order ? edge_b_edges[inner_i] :
                                                           edge_b_edges.last(inner_i);
      edge_face_edges[face_i + Corner::B] = face_edges_c[inner_face_edges.last_of(i)];
      edge_face_edges[face_i + Corner::C] = face_edges_a[inner_face_edges.last_of(r_i)];
    }

    for (const int i : faces_of_edge_range.index_range()) {
      const int r_i = faces_of_edge_range.last(i);
      const int inner_i = i + 1;
      const int face_i = faces_of_edge_range.step(InnerEdges::Right)[i] * face_size;
      edge_face_edges[face_i + Corner::A] = edge_c_order ? edge_c_edges[inner_i] :
                                                           edge_c_edges.last(inner_i);
      edge_face_edges[face_i + Corner::B] = face_edges_a[inner_face_edges.first_of(i)];
      edge_face_edges[face_i + Corner::C] = face_edges_b[inner_face_edges.last_of(r_i)];
    }

    /* Faces (flipped). */
    for (const int line_i : IndexRange(inner_top_faces_of_face.hight())) {
      BLI_assert(inner_top_faces_of_face.slice_at(line_i).size() ==
                 inner_face_edges.slice_at(line_i).size());
      MutableSpan<int> line = top_face_edges.slice(
          inner_top_faces_of_face.slice_at(line_i).scale(face_size));
      const IndexRange inner_edge_line = inner_face_edges.slice_at(line_i);
      const IndexRange range = inner_edge_line.index_range();
      for (const int i : range) {
        const int r_i = range.last(i);
        const int face_i = i * face_size;
        line[face_i + Corner::A] = face_edges_a[inner_edge_line[i]];
        line[face_i + Corner::B] = face_edges_b[inner_face_edges.first_of(r_i) + line_i];
        line[face_i + Corner::C] = face_edges_c[inner_face_edges.first_of(i) + line_i];
      }
    }

    /* Faces (non-flipped). */
    for (const int line_i : IndexRange(inner_bottom_faces_of_face.hight())) {
      BLI_assert(inner_bottom_faces_of_face.slice_at(line_i).size() ==
                 inner_face_edges.slice_at(line_i).size() - 2);
      MutableSpan<int> line = bottom_face_edges.slice(
          inner_bottom_faces_of_face.slice_at(line_i).scale(face_size));
      const IndexRange inner_edge_line = inner_face_edges.slice_at(line_i).drop_front(1).drop_back(
          1);
      const IndexRange range = inner_edge_line.index_range();
      const int r_line_i = inner_bottom_faces_of_face.hight() - line_i - 1;
      for (const int i : range) {
        const int face_i = i * face_size;
        const int inner_line_i = line_i + 1;
        line[face_i + Corner::A] = face_edges_a[inner_edge_line[i]];
        line[face_i + Corner::B] =
            face_edges_b[inner_face_edges.first_of(r_line_i - i) + inner_line_i];
        line[face_i + Corner::C] = face_edges_c[inner_face_edges.first_of(i) + inner_line_i];
      }
    }
  });
}

static void corner_verts_from_edges(const Span<int> corner_edges,
                                    const Span<int2> edges,
                                    const int faces_num,
                                    MutableSpan<int> corner_verts)
{
  const Span<int3> base_face_verts = base_face_verts_indices();
  if (base_face_verts.cast<int>().size() == corner_verts.size()) {
    corner_verts.copy_from(base_face_verts.cast<int>());
    return;
  }

  // SCOPED_TIMER_AVERAGED(__func__);
  threading::parallel_for(IndexRange(faces_num), 2048, [&](const IndexRange range) {
    for (const int64_t i : range) {
      const int face_i = i * face_size;
      const int2 edge_a = edges[corner_edges[face_i + FaceVert::A]];
      const int2 edge_b = edges[corner_edges[face_i + FaceVert::B]];
      const int2 edge_c = edges[corner_edges[face_i + FaceVert::C]];

      BLI_assert(elem_of(edge_a[EdgeVert::A], edge_b) != elem_of(edge_a[EdgeVert::A], edge_c));
      BLI_assert(elem_of(edge_b[EdgeVert::A], edge_a) != elem_of(edge_b[EdgeVert::A], edge_c));
      BLI_assert(elem_of(edge_c[EdgeVert::A], edge_b) != elem_of(edge_c[EdgeVert::A], edge_a));

      const int vert_a = elem_of(edge_a[EdgeVert::A], edge_c) ? edge_a[EdgeVert::A] :
                                                                edge_a[EdgeVert::B];
      const int vert_c = bke::mesh::edge_other_vert(edge_c, vert_a);
      const int vert_b = bke::mesh::edge_other_vert(edge_b, vert_c);

      corner_verts[face_i + FaceVert::A] = vert_a;
      corner_verts[face_i + FaceVert::B] = vert_b;
      corner_verts[face_i + FaceVert::C] = vert_c;
    }
  });
}

static void fill_uv_line_of_triangles(const float2 &begin_a,
                                      const float2 &begin_b,
                                      const float2 &begin_c,
                                      const float2 &end_a,
                                      const float2 &end_b,
                                      const float2 &end_c,
                                      MutableSpan<float2> triangles_uv)
{
  BLI_assert(triangles_uv.size() % 3 == 0);
  const int faces_num = triangles_uv.size() / 3;
  const float count = float(faces_num + 1);
  for (const int i : IndexRange(faces_num)) {
    const float factor = float(i + 1) / count;
    triangles_uv[i * face_size + Corner::A] = bke::attribute_math::mix2<float2>(
        factor, begin_a, end_a);
    triangles_uv[i * face_size + Corner::B] = bke::attribute_math::mix2<float2>(
        factor, begin_b, end_b);
    triangles_uv[i * face_size + Corner::C] = bke::attribute_math::mix2<float2>(
        factor, begin_c, end_c);
  }
}

static void uv_vert_positions(const int edge_edges_num,
                              const IndexRange faces_of_edge_range,
                              const IndexRange faces_of_face_range,
                              const IndexRange corner_faces_range,
                              const IndexRange edges_faces_range,
                              const IndexRange top_faces_range,
                              const IndexRange bottom_faces_range,
                              const TriangleRange inner_bottom_faces_of_face,
                              const TriangleRange inner_top_faces_of_face,
                              MutableSpan<float2> uv)
{
  // SCOPED_TIMER_AVERAGED(__func__);

  const Span<int3> base_face_verts = base_face_verts_indices();
  const Span<int3> base_faces_edges = base_face_edge_indices();

  const Span<float2> base_uv = base_face_uv_positions();
  if (uv.size() == base_uv.size()) {
    uv.copy_from(base_uv);
    return;
  }

  foreach_face(faces_of_face_range.size(), [&](const int face_i) {
    const int3 face_vert_indices = base_face_verts[face_i];
    const int3 face_edge_indices = base_faces_edges[face_i];

    MutableSpan<float2> face_uv = uv.slice(faces_of_face_range.step(face_i).scale(face_size));

    MutableSpan<float2> corner_face_edges_uv = face_uv.slice(corner_faces_range.scale(face_size));
    MutableSpan<float2> edge_face_edges_uv = face_uv.slice(edges_faces_range.scale(face_size));
    MutableSpan<float2> top_face_edges_uv = face_uv.slice(top_faces_range.scale(face_size));
    MutableSpan<float2> bottom_face_edges_uv = face_uv.slice(bottom_faces_range.scale(face_size));

    const constexpr IndexRange single_face_range(face_size);
    const float2 uv_corner_a = base_uv[single_face_range.step(face_i)[FaceVert::A]];
    const float2 uv_corner_b = base_uv[single_face_range.step(face_i)[FaceVert::B]];
    const float2 uv_corner_c = base_uv[single_face_range.step(face_i)[FaceVert::C]];

    const float2 uv_ab_edge = uv_corner_a - uv_corner_b;
    const float2 uv_bc_edge = uv_corner_b - uv_corner_c;
    const float2 uv_ca_edge = uv_corner_c - uv_corner_a;

    /* Faces in corners of base face. */
    MutableSpan<float2> a_corner_face_uv = corner_face_edges_uv.slice(
        single_face_range.step(Corner::B));
    a_corner_face_uv[Corner::A] = uv_corner_a;
    a_corner_face_uv[Corner::B] = uv_corner_a - uv_ab_edge / edge_edges_num;
    a_corner_face_uv[Corner::C] = uv_corner_a + uv_ca_edge / edge_edges_num;

    MutableSpan<float2> b_corner_face_uv = corner_face_edges_uv.slice(
        single_face_range.step(Corner::C));
    b_corner_face_uv[Corner::A] = uv_corner_b + uv_ab_edge / edge_edges_num;
    b_corner_face_uv[Corner::B] = uv_corner_b;
    b_corner_face_uv[Corner::C] = uv_corner_b - uv_bc_edge / edge_edges_num;

    MutableSpan<float2> c_corner_face_uv = corner_face_edges_uv.slice(
        single_face_range.step(Corner::A));
    c_corner_face_uv[Corner::A] = uv_corner_c - uv_ca_edge / edge_edges_num;
    c_corner_face_uv[Corner::B] = uv_corner_c + uv_bc_edge / edge_edges_num;
    c_corner_face_uv[Corner::C] = uv_corner_c;

    MutableSpan<float2> base_edge_faces_uv = edge_face_edges_uv.slice(
        faces_of_edge_range.step(InnerEdges::Base).scale(face_size));
    fill_uv_line_of_triangles(a_corner_face_uv[Corner::A],
                              a_corner_face_uv[Corner::B],
                              a_corner_face_uv[Corner::C],
                              b_corner_face_uv[Corner::A],
                              b_corner_face_uv[Corner::B],
                              b_corner_face_uv[Corner::C],
                              base_edge_faces_uv);

    MutableSpan<float2> left_edge_faces_uv = edge_face_edges_uv.slice(
        faces_of_edge_range.step(InnerEdges::Left).scale(face_size));
    fill_uv_line_of_triangles(c_corner_face_uv[Corner::B],
                              c_corner_face_uv[Corner::C],
                              c_corner_face_uv[Corner::A],
                              b_corner_face_uv[Corner::B],
                              b_corner_face_uv[Corner::C],
                              b_corner_face_uv[Corner::A],
                              left_edge_faces_uv);

    MutableSpan<float2> right_edge_faces_uv = edge_face_edges_uv.slice(
        faces_of_edge_range.step(InnerEdges::Right).scale(face_size));
    fill_uv_line_of_triangles(a_corner_face_uv[Corner::C],
                              a_corner_face_uv[Corner::A],
                              a_corner_face_uv[Corner::B],
                              c_corner_face_uv[Corner::C],
                              c_corner_face_uv[Corner::A],
                              c_corner_face_uv[Corner::B],
                              right_edge_faces_uv);

    /* Faces (flipped). */
    for (const int line_i : IndexRange(inner_top_faces_of_face.hight())) {
      const IndexRange line_range = inner_top_faces_of_face.slice_at(line_i);
      const IndexRange line_body_range = line_range.drop_front(1).drop_back(1);
      MutableSpan<float2> line_uv = top_face_edges_uv.slice(line_body_range.scale(face_size));
      MutableSpan<float2> line_begin_uv = top_face_edges_uv.slice(
          line_range.take_front(1).scale(face_size));
      MutableSpan<float2> line_end_uv = top_face_edges_uv.slice(
          line_range.take_back(1).scale(face_size));

      const int r_line_i = inner_top_faces_of_face.hight() - line_i - 1;
      const float factor = float(r_line_i) / float(inner_top_faces_of_face.hight());
      const float2 bottom_middle = math::midpoint(c_corner_face_uv[Corner::A],
                                                  c_corner_face_uv[Corner::B]);
      const float2 offset_to_mirror_c = (bottom_middle - c_corner_face_uv[Corner::C]) * 2.0f;
      line_begin_uv[Corner::A] = bke::attribute_math::mix2<float2>(
          factor, c_corner_face_uv[Corner::B], a_corner_face_uv[Corner::B]);
      line_begin_uv[Corner::B] = bke::attribute_math::mix2<float2>(
          factor, c_corner_face_uv[Corner::A], a_corner_face_uv[Corner::A]);
      line_begin_uv[Corner::C] = bke::attribute_math::mix2<float2>(
          factor, c_corner_face_uv[Corner::C], a_corner_face_uv[Corner::C]);
      line_begin_uv[Corner::C] += offset_to_mirror_c;

      /* This write in the end of current span. This slight distributed write. But using temporal
       * values to delay filing end will save just ~10%. */
      line_end_uv[Corner::A] = bke::attribute_math::mix2<float2>(
          factor, c_corner_face_uv[Corner::B], b_corner_face_uv[Corner::B]);
      line_end_uv[Corner::B] = bke::attribute_math::mix2<float2>(
          factor, c_corner_face_uv[Corner::A], b_corner_face_uv[Corner::A]);
      line_end_uv[Corner::C] = bke::attribute_math::mix2<float2>(
          factor, c_corner_face_uv[Corner::C], b_corner_face_uv[Corner::C]);
      line_end_uv[Corner::C] += offset_to_mirror_c;

      fill_uv_line_of_triangles(line_begin_uv[Corner::A],
                                line_begin_uv[Corner::B],
                                line_begin_uv[Corner::C],
                                line_end_uv[Corner::A],
                                line_end_uv[Corner::B],
                                line_end_uv[Corner::C],
                                line_uv);
    }

    /* Faces (non-flipped). */
    for (const int line_i : IndexRange(inner_bottom_faces_of_face.hight())) {
      const int r_line_i = inner_bottom_faces_of_face.hight() - line_i - 1;
      const int face_i = line_i * face_size;
      const int r_face_i = (r_line_i + 1) * face_size;
      const IndexRange line_range = inner_bottom_faces_of_face.slice_at(line_i);
      MutableSpan<float2> line_uv = bottom_face_edges_uv.slice(line_range.scale(face_size));
      fill_uv_line_of_triangles(right_edge_faces_uv[face_i + Corner::B],
                                right_edge_faces_uv[face_i + Corner::C],
                                right_edge_faces_uv[face_i + Corner::A],
                                left_edge_faces_uv[r_face_i + Corner::C],
                                left_edge_faces_uv[r_face_i + Corner::A],
                                left_edge_faces_uv[r_face_i + Corner::B],
                                line_uv);
    }
  });
}

Mesh *create_icosphere_mesh(const int resolution,
                            const float radius,
                            const std::optional<StringRef> &uv_id)
{
  // std::cout << std::endl;
  const int base_edge_verts_num = math::max<int>(0, resolution - 2);
  const int base_edge_edges_num = math::max<int>(0, resolution - 1);

  const TriangleRange face_of_verts(base_edge_verts_num);
  const TriangleRange face_of_edges(base_edge_edges_num);

  const TriangleRange inner_face_of_verts = face_of_verts.drop_bottom(1);
  const TriangleRange inner_face_edges = face_of_edges.drop_bottom(1);

  const TriangleRange bottom_faces_of_face(base_edge_edges_num);
  const TriangleRange top_faces_of_face(base_edge_edges_num - 1);

  /* Faces which is not connected to base edges/vertices.
   * Around such faces here is: corner_faces and edge_faces. */
  const TriangleRange inner_bottom_faces_of_face = bottom_faces_of_face.drop_bottom(3);
  const TriangleRange inner_top_faces_of_face = top_faces_of_face;

  const int base_face_faces_num = bottom_faces_of_face.total() + top_faces_of_face.total();

  const int verts_num = base_verts_num + base_edges_num * base_edge_verts_num +
                        base_faces_num * inner_face_of_verts.total();
  const int edges_num = base_edges_num * base_edge_edges_num +
                        base_faces_num * inner_face_edges.total() * 3;
  const int faces_num = base_faces_num * base_face_faces_num;
  const int corners_num = faces_num * face_size;

  Mesh *mesh = BKE_mesh_new_nomain(verts_num, edges_num, faces_num, corners_num);

  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  MutableSpan<int2> edges = mesh->edges_for_write();
  MutableSpan<int> corner_edges = mesh->corner_edges_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();

  offset_indices::fill_constant_group_size(3, 0, mesh->face_offsets_for_write());

  const IndexRange verts_of_edge_range(base_edge_verts_num);
  const IndexRange verts_of_face_range(inner_face_of_verts.total());

  const IndexRange verts_of_verts_range(base_verts_num);
  const IndexRange verts_of_edges_range = verts_of_verts_range.after(base_edges_num *
                                                                     base_edge_verts_num);
  const IndexRange verts_of_faces_range = verts_of_edges_range.after(base_faces_num *
                                                                     inner_face_of_verts.total());

  const IndexRange edges_of_edge_range(base_edge_edges_num);
  const IndexRange edges_of_face_range(inner_face_edges.total());

  const IndexRange edges_of_edges_range(base_edges_num * base_edge_edges_num);
  const IndexRange edges_of_faces_range = edges_of_edges_range.after(base_faces_num *
                                                                     inner_face_edges.total() * 3);

  /* All faces, not inner ones. */
  const IndexRange faces_of_face_range(base_face_faces_num);

  const int faces_of_edge_num = math::max<int>(0, base_edge_edges_num - 2);
  const IndexRange faces_of_edge_range(faces_of_edge_num);

  const IndexRange corner_faces_of_face_range(base_face_corners_num);
  const IndexRange edge_faces_of_face_range = corner_faces_of_face_range.after(
      faces_of_edge_num * base_face_corners_num);
  const IndexRange top_inner_faces_of_face_range = edge_faces_of_face_range.after(
      inner_top_faces_of_face.total());
  const IndexRange bottom_inner_faces_of_face_range = top_inner_faces_of_face_range.after(
      inner_bottom_faces_of_face.total());
  BLI_assert(
      ELEM(faces_of_face_range.size(), 1, bottom_inner_faces_of_face_range.one_after_last()));

  positions.take_front(base_verts_num).copy_from(base_ico_sphere_positions());
  interpolate_edge_verts_linear(base_edge_verts_num, positions.slice(verts_of_edges_range));
  interpolate_face_verts_linear(inner_face_of_verts, positions.slice(verts_of_faces_range));

  vert_edge_topology(verts_of_edge_range,
                     edges_of_edge_range,
                     verts_of_edges_range,
                     edges.slice(edges_of_edges_range));
  face_edge_topology(verts_of_edge_range,
                     verts_of_face_range,
                     edges_of_face_range,
                     verts_of_edges_range,
                     inner_face_of_verts,
                     inner_face_edges,
                     verts_of_faces_range,
                     edges.slice(edges_of_faces_range));

  corner_edges_topology(base_edge_edges_num,
                        edges_of_edge_range,
                        faces_of_edge_range,
                        faces_of_face_range,
                        corner_faces_of_face_range,
                        edge_faces_of_face_range,
                        top_inner_faces_of_face_range,
                        bottom_inner_faces_of_face_range,
                        inner_bottom_faces_of_face,
                        inner_top_faces_of_face,
                        inner_face_edges,
                        corner_edges);
  corner_verts_from_edges(corner_edges, edges, faces_num, corner_verts);

  {
    // SCOPED_TIMER_AVERAGED("new_old_normalize + Scaling");
    threading::parallel_for(positions.index_range(), 8192, [&](const IndexRange range) {
      MutableSpan<float3> slice = positions.slice(range);
      std::transform(slice.begin(), slice.end(), slice.begin(), [=](const float3 pos) {
        return math::normalize(pos) * radius;
      });
    });
  }

  if (uv_id.has_value()) {
    bke::SpanAttributeWriter<float2> uv_map = attributes.lookup_or_add_for_write_only_span<float2>(
        *uv_id, bke::AttrDomain::Corner);
    uv_vert_positions(base_edge_edges_num,
                      faces_of_edge_range,
                      faces_of_face_range,
                      corner_faces_of_face_range,
                      edge_faces_of_face_range,
                      top_inner_faces_of_face_range,
                      bottom_inner_faces_of_face_range,
                      inner_bottom_faces_of_face,
                      inner_top_faces_of_face,
                      uv_map.span);
    uv_map.finish();
  }

  BLI_assert(std::all_of(edges.cast<int>().begin(),
                         edges.cast<int>().end(),
                         [=](const int i) -> bool { return IndexRange(verts_num).contains(i); }));

  mesh->tag_loose_verts_none();
  mesh->tag_loose_edges_none();
  mesh->tag_overlapping_none();
  mesh->no_overlapping_topology();
  BKE_id_material_eval_ensure_default_slot(&mesh->id);

  bke::mesh_smooth_set(*mesh, false);

  debug_randomize_mesh_order(mesh);
  return mesh;
}

}  // namespace blender::geometry
