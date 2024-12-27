/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <algorithm>

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_sort.hh"
#include "BLI_vector.hh"

#include "BLI_polygon_clipping_2d.hh"

namespace blender::polygonboolean {

static int intersect(const float2 &P1,
                     const float2 &P2,
                     const float2 &Q1,
                     const float2 &Q2,
                     float *r_alpha_P,
                     float *r_alpha_Q)
{
  double r_lambda;
  double r_mu;
  const int val = isect_seg_seg_v2_lambda_mu_db(
      double2(P1), double2(P2), double2(Q1), double2(Q2), &r_lambda, &r_mu);

  *r_alpha_P = r_lambda;
  *r_alpha_Q = r_mu;

  return val;
}

static bool inside(const float2 &point, const Span<float2> poly)
{
  return isect_point_poly_v2(point, reinterpret_cast<const float(*)[2]>(poly.data()), poly.size());
}

static int point_in_polygon_winding_order(const float2 &point, const Span<float2> poly)
{
  /* TODO */
  return int(inside(point, poly));
}

// static bool seg_seg_winding(const float2 &P1, const float2 &P2, const float2 &Q1, const float2
// &Q2)
// {
//   /* TODO */
//   return 1;
// }

static ExtendedIntersectionPoint create_intersection(const int point_a,
                                                     const int point_b,
                                                     const float alpha_a,
                                                     const float alpha_b)
{
  ExtendedIntersectionPoint inter_point;
  inter_point.point_a = point_a;
  inter_point.point_b = point_b;
  inter_point.alpha_a = alpha_a;
  inter_point.alpha_b = alpha_b;

  return inter_point;
}

static constexpr bool EXIT = false;
static constexpr bool ENTRY = true;

static BooleanResult result_none()
{
  BooleanResult result;

  result.verts = {};
  result.offsets = {0};
  result.intersections_data = {};

  return result;
}

static BooleanResult result_from_curve(const Span<float2> curve, const bool is_curve_A)
{
  BooleanResult result;

  const int len = curve.size();

  result.verts = Array<Vertex>(len);
  result.offsets = {0, len};
  result.intersections_data = {};

  for (const int i : curve.index_range()) {
    result.verts[i] = {is_curve_A ? VertexType::PointA : VertexType::PointB, i};
  }

  return result;
}

static BooleanResult result_AB(const Span<float2> curve_a, const Span<float2> curve_b)
{
  BooleanResult result;

  const int len_a = curve_a.size();
  const int len_b = curve_b.size();

  result.verts = Array<Vertex>(len_a + len_b);
  result.offsets = {0, len_a, len_a + len_b};
  result.intersections_data = {};

  for (const int i : curve_a.index_range()) {
    result.verts[i] = {VertexType::PointA, i};
  }

  for (const int i : curve_b.index_range()) {
    result.verts[i + len_a] = {VertexType::PointB, i};
  }

  return result;
}

static BooleanResult result_BA(const Span<float2> curve_a, const Span<float2> curve_b)
{
  BooleanResult result;

  const int len_a = curve_a.size();
  const int len_b = curve_b.size();

  result.verts = Array<Vertex>(len_b + len_a);
  result.offsets = {0, len_b, len_b + len_a};
  result.intersections_data = {};

  for (const int i : curve_b.index_range()) {
    result.verts[i] = {VertexType::PointB, i};
  }

  for (const int i : curve_a.index_range()) {
    result.verts[i + len_b] = {VertexType::PointA, i};
  }

  return result;
}

static BooleanResult non_intersecting_result(const Operation mode,
                                             const Span<float2> curve_a,
                                             const Span<float2> curve_b)
{
  const bool a_start_in_b = inside(curve_a.first(), curve_b);
  const bool b_start_in_a = inside(curve_b.first(), curve_a);

  if (a_start_in_b) {
    switch (mode) {
      case Operation::And:
        return result_from_curve(curve_a, true);
      case Operation::Not:
        return result_none();
      case Operation::Or:
        return result_from_curve(curve_b, false);
      default:
        BLI_assert_unreachable();
        break;
    }
  }
  else if (b_start_in_a) {
    switch (mode) {
      case Operation::And:
        return result_from_curve(curve_b, false);
      case Operation::Not:
        return result_AB(curve_a, curve_b);
      case Operation::Or:
        return result_from_curve(curve_a, true);
      default:
        BLI_assert_unreachable();
        break;
    }
  }
  else if (!a_start_in_b && !b_start_in_a) {
    switch (mode) {
      case Operation::And:
        return result_none();
      case Operation::Not:
        return result_from_curve(curve_a, true);
      case Operation::Or:
        return result_AB(curve_a, curve_b);
      default:
        BLI_assert_unreachable();
        break;
    }
  }

  BLI_assert_unreachable();
  return result_none();
}

BooleanResult invalid_result(const Operation mode,
                             const Span<float2> curve_a,
                             const Span<float2> curve_b)
{
  switch (mode) {
    case Operation::And:
      return result_AB(curve_a, curve_b);
    case Operation::Not:
      return result_from_curve(curve_a, true);
    case Operation::Or:
      return result_AB(curve_a, curve_b);
    default:
      BLI_assert_unreachable();
      break;
  }

  BLI_assert_unreachable();
  return result_none();
}

static std::pair<bool, bool> get_AB_mode(const Operation mode)
{
  switch (mode) {
    case Operation::And:
      return {false, false};
    case Operation::Not:
      return {true, false};
    case Operation::Or:
      return {true, true};
    default:
      BLI_assert_unreachable();
      return {false, false};
  }
}

static bool is_point_in_others(const int polygon_a,
                               const OffsetIndices<int> points_by_polygon,
                               const Span<float2> points)
{
  const float2 &point = points[points_by_polygon[polygon_a].first()];
  for (const int polygon_b : points_by_polygon.index_range()) {
    if (polygon_b == polygon_a) {
      continue;
    }
    if (inside(point, points.slice(points_by_polygon[polygon_b]))) {
      return true;
    }
  }
  return false;
}

void interpolate_position_ab(const Span<float2> pos_a,
                             const Span<float2> pos_b,
                             const BooleanResult &result,
                             MutableSpan<float2> dst_pos)
{
  for (const int i : result.verts.index_range()) {
    const Vertex &vert = result.verts[i];
    const VertexType &type = vert.type;

    if (type == VertexType::PointA) {
      dst_pos[i] = pos_a[vert.point_id];
    }
    else if (type == VertexType::PointB) {
      dst_pos[i] = pos_b[vert.point_id];
    }
    else if (type == VertexType::Intersection) {
      const IntersectionPoint &inter_point = result.intersections_data[vert.point_id];

      const float2 a0 = pos_a[inter_point.point_a];
      const float2 a1 = pos_a[(inter_point.point_a + 1) % pos_a.size()];
      const float alpha_a = inter_point.alpha_a;

      dst_pos[i] = math::interpolate(a0, a1, alpha_a);
    }
  }
}

void interpolate_position_a(const Span<float2> pos_a,
                            const BooleanResult &result,
                            MutableSpan<float2> dst_pos)
{
  for (const int i : result.verts.index_range()) {
    const Vertex &vert = result.verts[i];
    const VertexType &type = vert.type;

    if (type == VertexType::PointA) {
      dst_pos[i] = pos_a[vert.point_id];
    }
    else if (type == VertexType::PointB) {
      dst_pos[i] = pos_a.first();
    }
    else if (type == VertexType::Intersection) {
      const IntersectionPoint &inter_point = result.intersections_data[vert.point_id];

      const float2 a0 = pos_a[inter_point.point_a];
      const float2 a1 = pos_a[(inter_point.point_a + 1) % pos_a.size()];
      const float alpha_a = inter_point.alpha_a;

      dst_pos[i] = math::interpolate(a0, a1, alpha_a);
    }
  }
}

static int result_find_base_id(const BooleanResult &result,
                               const Span<float2> curve_a,
                               const Span<float2> curve_b)
{
  const OffsetIndices<int> points_by_polygon = OffsetIndices<int>(result.offsets);
  Array<float2> points(result.verts.size());
  interpolate_position_ab(curve_a, curve_b, result, points.as_mutable_span());

  Vector<int> base_ids;

  /**
   * The base is the polygon that all others are inside of, and therefor it is not in any others.
   */
  for (const int polygon_a : points_by_polygon.index_range()) {
    if (!is_point_in_others(polygon_a, points_by_polygon, points)) {
      base_ids.append(polygon_a);
    }
  }

  if (base_ids.size() == 1) {
    return base_ids.first();
  }

  return -1;
}

BooleanResult result_remove_holes(const BooleanResult &in_result,
                                  const Span<float2> curve_a,
                                  const Span<float2> curve_b)
{
  BooleanResult result;

  const int base_id = result_find_base_id(in_result, curve_a, curve_b);

  if (base_id == -1) {
    return in_result;
  }

  const int base_start = in_result.offsets[base_id];
  const int base_end = in_result.offsets[base_id + 1];
  const int base_size = base_end - base_start;

  Array<Vertex> verts(base_size);
  const Array<int> offsets = {0, base_size};

  const int og_intersection_size = in_result.intersections_data.size();
  Array<int> reverse_map(og_intersection_size, -1);
  int new_intersection_size = 0;

  for (const int i : IndexRange(base_size)) {
    const Vertex &vert = in_result.verts[i + base_start];
    if (vert.type != VertexType::Intersection) {
      verts[i] = vert;
      continue;
    }

    if (reverse_map[vert.point_id] == -1) {
      reverse_map[vert.point_id] = new_intersection_size;
      new_intersection_size++;
    }
    verts[i] = {VertexType::Intersection, reverse_map[vert.point_id]};
  }

  Array<IntersectionPoint> intersections(new_intersection_size);
  for (const int i : IndexRange(og_intersection_size)) {
    if (reverse_map[i] != -1) {
      intersections[reverse_map[i]] = in_result.intersections_data[i];
    }
  }

  result.verts = verts;
  result.offsets = offsets;
  result.intersections_data = intersections;

  return result;
}

BooleanResult result_sort_holes(const BooleanResult &in_result,
                                const Span<float2> curve_a,
                                const Span<float2> curve_b)
{
  BooleanResult result;

  const int base_id = result_find_base_id(in_result, curve_a, curve_b);

  if (base_id == -1) {
    return in_result;
  }

  const OffsetIndices<int> points_by_polygon = OffsetIndices<int>(in_result.offsets);
  const IndexRange polygons_before_base = IndexRange(base_id);
  const IndexRange polygons_after_base = IndexRange::from_begin_end(base_id + 1,
                                                                    points_by_polygon.size());

  const IndexRange base_points = points_by_polygon[base_id];

  Array<Vertex> verts(in_result.verts.size());
  Array<int> offsets(in_result.offsets.size());

  int i = 0;
  int curr_off = 0;

  offsets[curr_off++] = i;
  for (const int j : base_points.index_range()) {
    verts[i++] = in_result.verts[base_points[j]];
  }

  for (const int polygon_j : polygons_before_base.index_range()) {
    offsets[curr_off++] = i;
    for (const int j : points_by_polygon[polygon_j]) {
      verts[i++] = in_result.verts[j];
    }
  }
  for (const int polygon_j : polygons_after_base.index_range()) {
    offsets[curr_off++] = i;
    for (const int j : points_by_polygon[polygon_j]) {
      verts[i++] = in_result.verts[j];
    }
  }

  offsets[curr_off++] = i;

  result.verts = verts;
  result.offsets = offsets;
  result.intersections_data = in_result.intersections_data;

  return result;
}

/**
 * Utility class to avoid passing large number of parameters between functions.
 */
struct CurveBooleanExecutor {
  int len_a;
  int len_b;

  Vector<ExtendedIntersectionPoint> intersections;
  int num_intersects;

  Array<int> A_inter_sorted_ids;
  Array<int> B_inter_sorted_ids;

  Vector<Vertex> verts;
  Vector<int> vertex_offsets;

  void new_polygon()
  {
    vertex_offsets.append(verts.size());
  }

  void new_vertex_id(int point_id, bool is_curve_A)
  {
    verts.append({is_curve_A ? VertexType::PointA : VertexType::PointB, point_id});
  }

  void new_vertex_intersection(int point_id)
  {
    verts.append({VertexType::Intersection, point_id});
  }

  int get_next_intersection_id(int id0, bool is_curve_A)
  {
    const ExtendedIntersectionPoint &vertex0 = intersections[id0];

    const int int_sorted = is_curve_A ? vertex0.sorted_id_a : vertex0.sorted_id_b;
    const bool direction = is_curve_A ? vertex0.A_entry_exit : vertex0.B_entry_exit;

    const int next_sorted_int = (int_sorted + (direction == ENTRY ? 1 : num_intersects - 1)) %
                                num_intersects;

    return is_curve_A ? A_inter_sorted_ids[next_sorted_int] : B_inter_sorted_ids[next_sorted_int];
  }

  void add_between_points(int id0, bool is_curve_A)
  {
    const int id1 = get_next_intersection_id(id0, is_curve_A);

    const ExtendedIntersectionPoint &vertex0 = intersections[id0];
    const ExtendedIntersectionPoint &vertex1 = intersections[id1];

    const bool direction = is_curve_A ? vertex0.A_entry_exit : vertex0.B_entry_exit;

    const int i0 = is_curve_A ? vertex0.point_a : vertex0.point_b;
    const int i1 = is_curve_A ? vertex1.point_a : vertex1.point_b;

    const int curve_len = is_curve_A ? len_a : len_b;

    /* If both intersection points are on the same segment, there's ether no points between or
     * all of the points. */
    if (i0 == i1) {
      const float a0 = is_curve_A ? vertex0.alpha_a : vertex0.alpha_b;
      const float a1 = is_curve_A ? vertex1.alpha_a : vertex1.alpha_b;

      if (direction == ENTRY && a0 < a1) {
        return;
      }
      if (direction == EXIT && a0 > a1) {
        return;
      }
    }

    /* Both start and end are included. */
    int start = i0;
    int end = i1;

    /* Add one to not include the point past the segment. */
    if (direction == ENTRY) {
      start = i0 + 1;
    }
    else { /* direction == EXIT */
      end = i1 + 1;
    }

    /* When a full loop is need. */
    if (i0 >= i1 && direction == ENTRY) {
      end = i1 + curve_len;
    }
    if (i0 <= i1 && direction == EXIT) {
      start = i0 + curve_len;
    }

    if (direction == ENTRY) {
      for (int i = start; i <= end; i++) {
        this->new_vertex_id(i % curve_len, is_curve_A);
      }
    }
    else { /* direction == EXIT */
      for (int i = start; i >= end; i--) {
        this->new_vertex_id(i % curve_len, is_curve_A);
      }
    }
  }

  BooleanResult copy_data_to_result()
  {
    Array<Vertex> verts_out(verts.size());
    array_utils::copy(verts.as_span(), verts_out.as_mutable_span());

    Array<int> offsets(vertex_offsets.size());
    array_utils::copy(vertex_offsets.as_span(), offsets.as_mutable_span());

    Array<IntersectionPoint> intersections_data(intersections.size());
    for (const int i : intersections.index_range()) {
      const ExtendedIntersectionPoint &inter = intersections[i];
      intersections_data[i] = {inter.point_a, inter.point_b, inter.alpha_a, inter.alpha_b};
    }

    BooleanResult result;

    result.verts = verts_out;
    result.offsets = offsets;
    result.intersections_data = intersections_data;

    return result;
  }

  void sort_a_intersections()
  {
    A_inter_sorted_ids = Array<int>(num_intersects);
    array_utils::fill_index_range<int>(A_inter_sorted_ids);

    parallel_sort(A_inter_sorted_ids.begin(), A_inter_sorted_ids.end(), [&](int i1, int i2) {
      return intersections[i1].point_a + intersections[i1].alpha_a <
             intersections[i2].point_a + intersections[i2].alpha_a;
    });

    for (const int id : IndexRange(num_intersects)) {
      intersections[A_inter_sorted_ids[id]].sorted_id_a = id;
    }
  }

  void sort_b_intersections()
  {
    B_inter_sorted_ids = Array<int>(num_intersects);
    array_utils::fill_index_range<int>(B_inter_sorted_ids);

    parallel_sort(B_inter_sorted_ids.begin(), B_inter_sorted_ids.end(), [&](int i1, int i2) {
      return intersections[i1].point_b + intersections[i1].alpha_b <
             intersections[i2].point_b + intersections[i2].alpha_b;
    });

    for (const int id : IndexRange(num_intersects)) {
      intersections[B_inter_sorted_ids[id]].sorted_id_b = id;
    }
  }

  void set_a_directions(const Span<float2> curve_a, const Span<float2> curve_b, bool A_mode)
  {
    const bool a_start_in_b = inside(curve_a.first(), curve_b);

    bool A_status = a_start_in_b ^ A_mode ? EXIT : ENTRY;

    for (const int j : IndexRange(num_intersects)) {
      intersections[A_inter_sorted_ids[j]].A_entry_exit = A_status;

      A_status = !A_status;
    }
  }

  void set_b_directions(const Span<float2> curve_a, const Span<float2> curve_b, bool B_mode)
  {
    const bool b_start_in_a = inside(curve_b.first(), curve_a);

    bool B_status = b_start_in_a ^ B_mode ? EXIT : ENTRY;

    for (const int j : IndexRange(num_intersects)) {
      intersections[B_inter_sorted_ids[j]].B_entry_exit = B_status;

      B_status = !B_status;
    }
  }

  /**
   * This is a modified implementation of the Greiner-Hormann clipping algorithm.
   *
   * Greiner, Günther; Kai Hormann (1998). "Efficient clipping of arbitrary polygons". ACM
   * Transactions on Graphics. 17 (2): 71-83.
   *
   * The Greiner-Hormann algorithm works in three phases:
   *  1: Find all intersections and sort them.
   *  2: Set the `direction` of all intersection point (the paper call it `entry_exit`)
   *  3: Create all polygons by following the direction of each intersection point until in
   * loops.
   *
   * Some changes where made to use c++ style objects and so that the intersection point stored
   * separately from the rest of the points.
   *
   * The original algorithm created two copies of each intersection point, one in the curve `A`
   * and the other in curve `B`. each would be sorted and have pointer to the other.
   * Instead we store each intersection point in an arbitrarily ordered list and then have two
   * lists of sorted indices that point to intersection point for curve `A` and `B`
   */
  std::optional<BooleanResult> execute_boolean(const Operation boolean_mode,
                                               const Span<float2> curve_a,
                                               const Span<float2> curve_b)
  {
    len_a = curve_a.size();
    len_b = curve_b.size();

    /* ---- ---- ---- Phase One ---- ---- ---- */

    for (const int i : curve_a.index_range()) {
      for (const int j : curve_b.index_range()) {
        float alpha_a, alpha_b;
        const int val = intersect(curve_a[i],
                                  curve_a[(i + 1) % len_a],
                                  curve_b[j],
                                  curve_b[(j + 1) % len_b],
                                  &alpha_a,
                                  &alpha_b);
        if (val == ISECT_LINE_LINE_CROSS) {
          intersections.append(create_intersection(i, j, alpha_a, alpha_b));
        }
        else if (val == ISECT_LINE_LINE_EXACT) {
          return std::nullopt;
        }
      }
    }

    if (intersections.is_empty()) {
      return std::optional(non_intersecting_result(boolean_mode, curve_a, curve_b));
    }

    num_intersects = intersections.size();

    sort_a_intersections();
    sort_b_intersections();

    /* ---- ---- ---- Phase Two ---- ---- ---- */

    const auto [A_mode, B_mode] = get_AB_mode(boolean_mode);

    set_a_directions(curve_a, curve_b, A_mode);
    set_b_directions(curve_a, curve_b, B_mode);

    /* ---- ---- ---- Phase Three ---- ---- ---- */

    Array<bool> unprocessed_intersection_unsorted_ids(num_intersects, true);
    int inter_id = 0;

    while (inter_id != -1) {
      bool is_curve_A = true;

      /* Both of these are in unsorted space. */
      int curr_int_id = inter_id;
      const int start_int_id = curr_int_id;

      this->new_polygon();

      bool PolygonClosed = false;
      while (!PolygonClosed) {
        add_between_points(curr_int_id, is_curve_A);

        curr_int_id = get_next_intersection_id(curr_int_id, is_curve_A);
        unprocessed_intersection_unsorted_ids[curr_int_id] = false;

        /* Switch to the other curve. */
        is_curve_A = !is_curve_A;

        this->new_vertex_intersection(curr_int_id);

        if (is_curve_A && curr_int_id == start_int_id) {
          PolygonClosed = true;
        }
      }

      /* Get the next unprocessed intersection point (in unsorted space). */
      inter_id = unprocessed_intersection_unsorted_ids.as_span().first_index_try(true);
    }

    /* Add one for the end. */
    this->new_polygon();

    return std::optional(copy_data_to_result());
  }

  bool cut_add_start_cap()
  {
    const int inter_id = A_inter_sorted_ids.first();
    const ExtendedIntersectionPoint &vertex_first = intersections[inter_id];

    if (vertex_first.A_entry_exit == ENTRY) {
      return false;
    }

    /* Start line. */
    this->new_polygon();

    for (const int i : IndexRange::from_begin_end_inclusive(0, vertex_first.point_a)) {
      this->new_vertex_id(i, true);
    }

    this->new_vertex_intersection(inter_id);
    return true;
  }

  bool cut_add_end_cap()
  {
    const int inter_id = A_inter_sorted_ids.last();
    const ExtendedIntersectionPoint &vertex_last = intersections[inter_id];

    if (vertex_last.A_entry_exit == EXIT) {
      return false;
    }

    /* Start line. */
    this->new_polygon();

    this->new_vertex_intersection(inter_id);

    for (const int i : IndexRange::from_begin_end(vertex_last.point_a + 1, len_a)) {
      this->new_vertex_id(i, true);
    }
    return true;
  }

  void cut_add_between_points(const int int_sorted)
  {
    const int i0 = A_inter_sorted_ids[int_sorted];
    const int i1 = A_inter_sorted_ids[int_sorted + 1];

    const ExtendedIntersectionPoint &vertex0 = intersections[i0];
    const ExtendedIntersectionPoint &vertex1 = intersections[i1];

    BLI_assert(vertex0.A_entry_exit == ENTRY);

    /* Start line. */
    this->new_polygon();

    this->new_vertex_intersection(i0);

    if (vertex0.point_a != vertex1.point_a) {
      for (const int i :
           IndexRange::from_begin_end_inclusive(vertex0.point_a + 1, vertex1.point_a))
      {
        this->new_vertex_id(i, true);
      }
    }

    this->new_vertex_intersection(i1);
  }

  bool cut_add_looping_cap()
  {
    const int inter_id_first = A_inter_sorted_ids.first();
    const int inter_id_last = A_inter_sorted_ids.last();
    const ExtendedIntersectionPoint &vertex_first = intersections[inter_id_first];
    const ExtendedIntersectionPoint &vertex_last = intersections[inter_id_last];

    if (vertex_first.A_entry_exit == ENTRY || vertex_last.A_entry_exit == EXIT) {
      return false;
    }

    /* Start line. */
    this->new_polygon();

    /* Start at the last intersection point. */
    this->new_vertex_intersection(inter_id_last);

    /* Go through all point from last intersection point to the end. */
    for (const int i : IndexRange::from_begin_end(vertex_last.point_a + 1, len_a)) {
      this->new_vertex_id(i, true);
    }

    /* Go through all point from start point to the first intersection point. */
    for (const int i : IndexRange::from_begin_end_inclusive(0, vertex_first.point_a)) {
      this->new_vertex_id(i, true);
    }

    /* End at the first intersection point. */
    this->new_vertex_intersection(inter_id_first);
    return true;
  }

  /* Curve `A` does not have fill. */
  std::optional<BooleanResult> execute_cut(const bool is_a_cyclic,
                                           const Span<float2> curve_a,
                                           const Span<float2> curve_b)
  {
    len_a = curve_a.size();
    len_b = curve_b.size();

    /* ---- ---- ---- Phase One ---- ---- ---- */

    const IndexRange a_range = is_a_cyclic ? IndexRange(len_a) : IndexRange(len_a - 1);

    for (const int i : a_range) {
      for (const int j : curve_b.index_range()) {
        float alpha_a, alpha_b;
        const int val = intersect(curve_a[i],
                                  curve_a[(i + 1) % len_a],
                                  curve_b[j],
                                  curve_b[(j + 1) % len_b],
                                  &alpha_a,
                                  &alpha_b);
        if (val == ISECT_LINE_LINE_CROSS) {
          intersections.append(create_intersection(i, j, alpha_a, alpha_b));
        }
        else if (val == ISECT_LINE_LINE_EXACT) {
          return std::nullopt;
        }
      }
    }

    if (intersections.is_empty()) {
      const bool a_start_in_b = inside(curve_a.first(), curve_b);
      if (a_start_in_b) {
        return std::optional(result_none());
      }
      else {
        return std::optional(result_from_curve(curve_a, true));
      }
    }

    num_intersects = intersections.size();

    sort_a_intersections();

    /* ---- ---- ---- Phase Two ---- ---- ---- */

    set_a_directions(curve_a, curve_b, true);

    /* ---- ---- ---- Phase Three ---- ---- ---- */

    bool is_looping = false;
    if (is_a_cyclic) {
      is_looping = cut_add_looping_cap();
    }

    bool is_start = is_looping;
    if (!is_looping) {
      is_start = cut_add_start_cap();
    }

    for (int i = is_start ? 1 : 0; i < num_intersects - 1; i += 2) {
      cut_add_between_points(i);
    }
    if (!is_looping) {
      cut_add_end_cap();
    }

    /* Add one for the end. */
    this->new_polygon();

    return std::optional(copy_data_to_result());
  }
};

std::optional<BooleanResult> curve_boolean_calc_old(const Operation boolean_mode,
                                                    const Span<float2> curve_a,
                                                    const Span<float2> curve_b)
{
  CurveBooleanExecutor executor;
  return executor.execute_boolean(boolean_mode, curve_a, curve_b);
}

std::optional<BooleanResult> curve_boolean_cut(const bool is_a_cyclic,
                                               const Span<float2> curve_a,
                                               const Span<float2> curve_b)
{
  CurveBooleanExecutor executor;
  return executor.execute_cut(is_a_cyclic, curve_a, curve_b);
}

/* TODO */
/* Will return -1 if there is no next segment. */
static int get_next_segment(const Span<Segment> unsorted_segments,
                            const int current_segment,
                            const Span<bool> processed_segments,
                            bool *r_reverse_next)
{
  if (!unsorted_segments[current_segment].has_end_intersection()) {
    return -1;
  }

  const int current_end_index = unsorted_segments[current_segment].end_intersection();

  for (const int segment : unsorted_segments.index_range()) {
    if (segment == current_segment || processed_segments[segment]) {
      continue;
    }

    if (unsorted_segments[segment].start_intersection() == current_end_index) {
      *r_reverse_next = unsorted_segments[segment].reversed;
      return segment;
    }

    if (unsorted_segments[segment].end_intersection() == current_end_index) {
      *r_reverse_next = !unsorted_segments[segment].reversed;
      return segment;
    }
  }

  return -1;
}

static bool winding_rule(const int winding_order)
{
  if (true) { /* TODO */
    return winding_order % 2 == 0;
  }
  else {
    return winding_order == 0;
  }
}

static bool contributing_rule(const int winding_order,
                              const bool is_subj,
                              const Operation boolean_mode)
{
  const bool is_fill = winding_rule(winding_order);

  switch (boolean_mode) {
    case Operation::And: {
      return !is_fill;
    }
    case Operation::Not: {
      return !is_fill ^ is_subj;
    }
    case Operation::Or: {
      return is_fill;
    }
    default:
      BLI_assert_unreachable();
      break;
  }

  return false;
}

static void calculate_offsets_from_segments(const Span<Segment> segments,
                                            const OffsetIndices<int> segment_offsets,
                                            const Span<bool> cyclic,
                                            MutableSpan<int> offsets)
{
  int offset = 0;

  for (const int curve_i : segment_offsets.index_range()) {
    offsets[curve_i] = offset;

    const IndexRange segment_range = segment_offsets[curve_i];
    for (const int seg_i : segment_range) {
      const Segment &segment = segments[seg_i];

      if (segment.has_start_intersection()) {
        offset++;
      }
      offset += segment.points_num();
      if (seg_i == segment_range.last() && segment.has_end_intersection() && !cyclic[curve_i]) {
        offset++;
      }
    }
  }

  offsets.last() = offset;
}

void calculate_positions(const Span<float2> pos_a,
                         const Span<float2> pos_b,
                         const BooleanResult &result,
                         MutableSpan<float2> dst_pos)
{
  const OffsetIndices<int> segments_by_polygon = OffsetIndices<int>(result.segment_offsets);
  int i = 0;

  for (const int curve_i : segments_by_polygon.index_range()) {
    const IndexRange segment_range = segments_by_polygon[curve_i];
    for (const int seg_i : segment_range) {
      const Segment &segment = result.segments[seg_i];

      const Span<float2> current_curve = (segment.curve == 0) ? pos_a : pos_b;

      if (segment.has_start_intersection()) {
        dst_pos[i++] = math::interpolate(current_curve[segment.start_edge().x],
                                         current_curve[segment.start_edge().y],
                                         segment.start_alpha());
      }

      segment.foreach_point(
          [&](const int index, const int pos) { dst_pos[pos + i] = current_curve[index]; });

      i += segment.points_num();

      if (seg_i == segment_range.last() && segment.has_end_intersection() &&
          !result.cyclic[curve_i])
      {
        dst_pos[i++] = math::interpolate(current_curve[segment.end_edge().x],
                                         current_curve[segment.end_edge().y],
                                         segment.end_alpha());
      }
    }
  }
}

std::optional<BooleanResult> execute_boolean(const Operation boolean_mode,
                                             const Span<float2> curve_a,
                                             const Span<float2> curve_b)
{
  const int num_curves = 2;
  Array<IndexRange> points_per_curve({curve_a.index_range(), curve_b.index_range()});
  Array<bool> input_cyclic({true, true});

  Vector<ExtendedIntersectionPoint> intersections;
  Array<Vector<int>> inters_per_curves(num_curves);

  /* Calculate all intersections. */
  {
    const int curve_i = 0;
    const int curve_j = 1;

    const IndexRange points_i = points_per_curve[curve_i];
    const IndexRange points_j = points_per_curve[curve_j];

    for (const int i : points_i.index_range()) {
      for (const int j : points_j.index_range()) {
        float alpha_a, alpha_b;
        const int val = intersect(curve_a[points_i[i]],
                                  curve_a[points_i[(i + 1) % points_i.size()]],
                                  curve_b[points_j[j]],
                                  curve_b[points_j[(j + 1) % points_j.size()]],
                                  &alpha_a,
                                  &alpha_b);
        if (val == ISECT_LINE_LINE_CROSS) {
          inters_per_curves[curve_i].append(intersections.size());
          inters_per_curves[curve_j].append(intersections.size());
          intersections.append(create_intersection(i, j, alpha_a, alpha_b));
        }
        else if (val == ISECT_LINE_LINE_EXACT) {
          return std::nullopt;
        }
      }
    }
  }

  if (intersections.is_empty()) {
    /* TODO */
    return std::optional(non_intersecting_result(boolean_mode, curve_a, curve_b));
  }

  /* Create all segments that are not internal. */
  Vector<Segment> unsorted_segments;
  for (const int curve_i : IndexRange(num_curves)) {
    const IndexRange points = points_per_curve[curve_i];
    const Vector<int> &inters_per_curve = inters_per_curves[curve_i];

    const bool is_subj = curve_i == 0;

    int current_winding_order = 0;

    /* TODO */
    const Span<float2> poly_this = (curve_i == 0) ? curve_a : curve_b;
    const Span<float2> poly_other = (curve_i == 0) ? curve_b : curve_a;

    current_winding_order = point_in_polygon_winding_order(poly_this.first(), poly_other);

    if (inters_per_curve.is_empty() &&
        contributing_rule(current_winding_order, is_subj, boolean_mode))
    {
      if (input_cyclic[curve_i]) {
        unsorted_segments.append(Segment::from_loop(curve_i, points));
      }
      else {
        unsorted_segments.append(Segment::from_start_to_end(curve_i, points));
      }
      continue;
    }

    Array<int> inter_sorted_ids = Array<int>(inters_per_curve.size());
    array_utils::fill_index_range<int>(inter_sorted_ids);

    parallel_sort(inter_sorted_ids.begin(), inter_sorted_ids.end(), [&](int i1, int i2) {
      const ExtendedIntersectionPoint &inter1 = intersections[inters_per_curve[i1]];
      const ExtendedIntersectionPoint &inter2 = intersections[inters_per_curve[i2]];
      if (curve_i == 0) { /* TODO */
        return inter1.point_a + inter1.alpha_a < inter2.point_a + inter2.alpha_a;
      }
      else {
        return inter1.point_b + inter1.alpha_b < inter2.point_b + inter2.alpha_b;
      }
    });

    if (input_cyclic[curve_i]) {
      if (contributing_rule(current_winding_order, is_subj, boolean_mode)) {
        const int int_p_1 = inters_per_curve[inter_sorted_ids.first()];
        const int int_p_2 = inters_per_curve[inter_sorted_ids.last()];

        const ExtendedIntersectionPoint &inter_first = intersections[int_p_1];
        const ExtendedIntersectionPoint &inter_last = intersections[int_p_2];

        unsorted_segments.append(Segment::from_intersections(
            curve_i, points, inter_last, inter_first, int_p_2, int_p_1));
      }
    }
    else {
      if (contributing_rule(current_winding_order, is_subj, boolean_mode)) {
        const ExtendedIntersectionPoint &inter_first =
            intersections[inters_per_curve[inter_sorted_ids.first()]];

        unsorted_segments.append(Segment::from_start_to_intersection(
            curve_i, points, inter_first, inters_per_curve[inter_sorted_ids.first()]));
      }
    }

    for (const int inter_id : inter_sorted_ids.index_range().drop_back(1)) {
      const int int_p_1 = inters_per_curve[inter_sorted_ids[inter_id]];
      const int int_p_2 = inters_per_curve[inter_sorted_ids[inter_id + 1]];

      const ExtendedIntersectionPoint &inter_first = intersections[int_p_1];

      current_winding_order++; /* TODO */
      // current_winding_order += seg_seg_winding(
      //     curve_a[inter_first.point_a],
      //     curve_a[(inter_first.point_a + 1) % curve_a.size()],
      //     curve_b[inter_first.point_b],
      //     curve_b[(inter_first.point_b + 1) % curve_b.size()]);

      if (contributing_rule(current_winding_order, is_subj, boolean_mode)) {
        const ExtendedIntersectionPoint &inter_last = intersections[int_p_2];

        unsorted_segments.append(Segment::from_intersections(
            curve_i, points, inter_first, inter_last, int_p_1, int_p_2));
      }
    }

    if (!input_cyclic[curve_i] && contributing_rule(current_winding_order, is_subj, boolean_mode))
    {
      const ExtendedIntersectionPoint &inter_last =
          intersections[inter_sorted_ids[inters_per_curve.last()]];

      unsorted_segments.append(Segment::from_intersection_to_end(
          curve_i, points, inter_last, inter_sorted_ids[inters_per_curve.last()]));
    }
  }

  /* Remove duplicate intersection points. */
  {
    /* TODO */
  }

  /* Follow each segment until it loops or ends. */
  Array<bool> processed_segments(unsorted_segments.size(), false);
  BooleanResult result;
  result.segment_offsets.append(0);

  int start_segment = processed_segments.first();

  while (start_segment != -1) {
    int current_segment = start_segment;

    bool PolygonDone = false;
    bool PolygonClosed = false;
    bool last_reversed = false;
    while (!PolygonDone) {
      if (processed_segments[current_segment] == true) {
        BLI_assert_unreachable();
        return std::nullopt;
      }

      unsorted_segments[current_segment].reversed = last_reversed;
      result.segments.append(unsorted_segments[current_segment]);
      processed_segments[current_segment] = true;
      result.segments.last().reversed = last_reversed;

      bool next_reversed;
      int next_segment = get_next_segment(
          unsorted_segments, current_segment, processed_segments, &next_reversed);

      if (next_segment == -1) {
        PolygonDone = true;
        break;
      }

      if (next_segment == start_segment) {
        PolygonDone = true;
        PolygonClosed = true;
      }

      last_reversed = next_reversed;
      current_segment = next_segment;
    }
    result.segment_offsets.append(result.segments.size());
    result.cyclic.append(PolygonClosed);

    /* Get the next unprocessed segment. */
    start_segment = processed_segments.as_span().first_index_try(false);
  }

  result.point_offsets.resize(result.segment_offsets.size());
  calculate_offsets_from_segments(result.segments,
                                  OffsetIndices<int>(result.segment_offsets),
                                  result.cyclic,
                                  result.point_offsets.as_mutable_span());

  return result;
}

std::optional<BooleanResult> curve_boolean_calc(const Operation boolean_mode,
                                                const Span<float2> curve_a,
                                                const Span<float2> curve_b)
{
  return execute_boolean(boolean_mode, curve_a, curve_b);
}

}  // namespace blender::polygonboolean
