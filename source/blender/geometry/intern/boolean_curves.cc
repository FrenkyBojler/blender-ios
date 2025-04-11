/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * This is a heavily modified implementation of the Greiner-Hormann clipping algorithm.
 *
 * Greiner, Günther; Kai Hormann (1998). "Efficient clipping of arbitrary polygons". ACM
 * Transactions on Graphics. 17 (2): 71-83.
 *
 * The original Greiner-Hormann algorithm works in three phases:
 *  1: Find all intersections and sort them.
 *  2: Set the direction of all intersection point (the paper call it `entry_exit`)
 *  3: Create all polygons by following the direction of each intersection point until it
 * loops.
 *
 * The original algorithm was only ever designed to work with one `subject` and one `clipping`
 * polygon, and with `OddEven` fill rule.
 *
 * This implementation adds the following:
 *  1: Groups of curves, called `shapes`. This allows for input geometry with holes.
 *  2: Curves can have no fill, so they will get cut.
 *  3: Multiple `clipping` shapes acting one `subject` shape.
 *  4: Separate fill rules for the `subject`, `clipping` and output geometry.
 *
 * This implementation works by:
 *  1: Break one subject shape and all clipping shapes into segments and store their intersections.
 *  2: Remove all segments that are not contributing.
 *  3: Follow each segment until it loops or terminates.
 *  4: Repeat for every `subject` shape.
 */

#include <algorithm>
#include <functional>

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_map.hh"
#include "BLI_math_base.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_sort.hh"
#include "BLI_vector.hh"

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_curves.hh"

#include "GEO_boolean_curves.hh"

namespace blender::geometry::boolean {

class Segment {
 public:
  int curve = -1;
  IndexRange points;

  int point_1 = -1;
  int point_2 = -1;

  float alpha_1 = 0.0;
  float alpha_2 = 0.0;

  int intersection_index[2] = {-1, -1};

  constexpr Segment() = default;

 public:
  bool is_loop() const
  {
    return alpha_2 == 1.0f;
  }

  bool has_start_intersection() const
  {
    return this->start_alpha() != 0.0f && this->start_alpha() != 1.0f;
  }

  bool has_end_intersection() const
  {
    return this->end_alpha() != 0.0f && this->end_alpha() != 1.0f;
  }

  float start_alpha() const
  {
    return alpha_1;
  }

  float end_alpha() const
  {
    return alpha_2;
  }

  int2 start_edge() const
  {
    return int2(point_1, this->wrap_index(point_1 + 1));
  }

  int2 end_edge() const
  {
    return int2(point_2, this->wrap_index(point_2 + 1));
  }

  int start_point() const
  {
    if (!this->has_start_intersection()) {
      return points.first();
    }
    return this->start_edge().y;
  }

  int end_point() const
  {
    return this->end_edge().x;
  }

  int wrap_index(const int i) const
  {
    return math::mod_periodic(i - points.first(), points.size()) + points.first();
  }

  IndexRange point_range() const
  {
    if (this->is_loop()) {
      return points;
    }

    if (!this->has_start_intersection() && this->has_end_intersection()) {
      return IndexRange::from_begin_end_inclusive(points.first(), point_2);
    }

    if (!this->has_start_intersection() && !this->has_end_intersection()) {
      return points;
    }

    /* If both intersection points are on the same edge, there's ether no points between or
     * all of the points are. */
    if (point_1 == point_2) {
      if (alpha_1 > alpha_2) {
        return points.shift(point_1 + 1);
      }
      return IndexRange(0);
    }

    if (point_1 > point_2) {
      return IndexRange::from_begin_end_inclusive(point_1 + 1, point_2 + points.size());
    }

    return IndexRange::from_begin_end_inclusive(point_1 + 1, point_2);
  }

  int points_num() const
  {
    return this->point_range().size();
  }

  template<typename Fn> inline void foreach_point(Fn &&fn) const
  {
    const IndexRange point_range = this->point_range();

    for (const int64_t pos : point_range.index_range()) {
      const int i = this->wrap_index(point_range[pos]);

      if constexpr (std::is_invocable_r_v<void, Fn, int64_t, int64_t>) {
        fn(i, pos);
      }
      else {
        fn(i);
      }
    }
  }

  constexpr static Segment from_curve(const int curve_i,
                                      const IndexRange points,
                                      const bool cyclical)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.points = points;

    segment.point_1 = points.first();
    segment.point_2 = points.last();

    if (cyclical) {
      segment.alpha_1 = 0.0f;
      segment.alpha_2 = 1.0f;
    }

    return segment;
  }

  static Segment from_intersections(const int curve_i,
                                    const IndexRange points,
                                    const float parameter_first,
                                    const float parameter_last,
                                    const int inter_index_first,
                                    const int inter_index_last)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.points = points;

    segment.point_1 = int(math::floor(parameter_first));
    segment.alpha_1 = math::fract(parameter_first);
    segment.intersection_index[0] = inter_index_first;

    segment.point_2 = int(math::floor(parameter_last));
    segment.alpha_2 = math::fract(parameter_last);
    segment.intersection_index[1] = inter_index_last;

    return segment;
  }

  static Segment from_start_to_intersection(const int curve_i,
                                            const IndexRange points,
                                            const float parameter_2,
                                            const int inter_index)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.points = points;

    segment.point_1 = points.first();

    segment.point_2 = int(math::floor(parameter_2));
    segment.alpha_2 = math::fract(parameter_2);
    segment.intersection_index[1] = inter_index;

    return segment;
  }

  static Segment from_intersection_to_end(const int curve_i,
                                          const IndexRange points,
                                          const float parameter_1,
                                          const int inter_index)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.points = points;

    segment.point_1 = int(math::floor(parameter_1));
    segment.alpha_1 = math::fract(parameter_1);

    segment.intersection_index[0] = inter_index;

    segment.point_2 = points.last();

    return segment;
  }
};

/**
 * -----------------------------------
 */

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

static float point_in_tri_winding(const float2 pt,
                                  const float2 v1,
                                  const float2 v2,
                                  const float2 v3)
{
  const float side12 = line_point_side_v2(v1, v2, pt);
  const float side23 = line_point_side_v2(v2, v3, pt);
  const float side31 = line_point_side_v2(v3, v1, pt);

  /* The point is on a corner. */
  if ((side12 == 0.0f && side23 == 0.0f) || (side23 == 0.0f && side31 == 0.0f) ||
      (side12 == 0.0f && side31 == 0.0f))
  {
    BLI_assert_unreachable();
    /* Note: The correct value would be the corner's signed angle, but the landing on an point is
     * error for the rest of the algorithm. */
    return 0.0f;
  }

  /* The point is on an edge. */
  if ((side12 == 0.0f && side23 >= 0.0f && side31 >= 0.0f) ||
      (side12 >= 0.0f && side23 == 0.0f && side31 >= 0.0f) ||
      (side12 >= 0.0f && side23 >= 0.0f && side31 == 0.0f))
  {
    return 0.5f;
  }
  if ((side12 == 0.0f && side23 <= 0.0f && side31 <= 0.0f) ||
      (side12 <= 0.0f && side23 == 0.0f && side31 <= 0.0f) ||
      (side12 <= 0.0f && side23 <= 0.0f && side31 == 0.0f))
  {
    return -0.5f;
  }

  /* The point is inside. */
  if (side12 >= 0.0f && side23 >= 0.0f && side31 >= 0.0f) {
    return 1.0f;
  }
  if (side12 <= 0.0f && side23 <= 0.0f && side31 <= 0.0f) {
    return -1.0f;
  }

  /* The point is outside. */
  return 0.0f;
}

/* Point must not be on a corner, but can be on an edge. */
static int point_in_polygon_winding_twice(const float2 &point, const Span<float2> poly)
{
  /* Double and store as a int to avoid float rounding. */
  int twice_winding = 0;
  const float2 &tri_p1 = poly[0];
  for (const int i : poly.index_range().drop_front(1).drop_back(1)) {
    const float2 &tri_p2 = poly[i];
    const float2 &tri_p3 = poly[i + 1];
    twice_winding += int(point_in_tri_winding(point, tri_p1, tri_p2, tri_p3) * 2);
  }
  return twice_winding;
}

/* Point must not be on a corner or edge. */
static int point_in_polygon_winding_int(const float2 &point, const Span<float2> poly)
{
  const int twice_winding = point_in_polygon_winding_twice(point, poly);
  BLI_assert(math::abs(twice_winding) % 2 == 0);
  return int(twice_winding / 2);
}

class WindingState {
 private:
  /* Winding order of each curve. */
  Map<int, int> orders_per_curve_;

 public:
  void add_to_curve(const int curve_id, const int winding_i)
  {
    if (winding_i == 0) {
      return;
    }

    if (orders_per_curve_.contains(curve_id)) {
      orders_per_curve_.lookup(curve_id) += winding_i;

      /* Remove unneeded ids. */
      if (orders_per_curve_.lookup(curve_id) == 0) {
        orders_per_curve_.remove(curve_id);
      }
    }
    else {
      orders_per_curve_.add(curve_id, winding_i);
    }
  }

  bool is_in_shape(const int shape_id,
                   const Vector<IndexMask> &shapes,
                   const FillRule fill_rule) const
  {
    const IndexMask &shape = shapes[shape_id];

    if (fill_rule == FillRule::NoHoles) {
      /* Each curve is checked individually. */
      return threading::parallel_reduce(
          shape.index_range(),
          4096,
          false,
          [&](const IndexRange range, bool value) {
            if (value) {
              return value;
            }
            shape.slice(range).foreach_index([&](const int curve_i) {
              if (orders_per_curve_.contains(curve_i)) {
                if (orders_per_curve_.lookup(curve_i) != 0) {
                  value = true;
                  return;
                }
              }
            });
            return value;
          },
          std::logical_or());
    }

    int winding = 0;

    shape.foreach_index([&](const int curve_i) {
      if (orders_per_curve_.contains(curve_i)) {
        winding += orders_per_curve_.lookup(curve_i);
      }
    });

    if (fill_rule == FillRule::EvenOdd) {
      return winding % 2 != 0;
    }
    else if (fill_rule == FillRule::NonZero) {
      return winding != 0;
    }

    BLI_assert_unreachable();
    return false;
  }

  bool is_in_shapes(const IndexMask &shapes_mask,
                    const Vector<IndexMask> &shapes,
                    const FillRule fill_rule) const
  {
    if (orders_per_curve_.is_empty() || shapes_mask.is_empty()) {
      return false;
    }

    return threading::parallel_reduce(
        shapes_mask.index_range(),
        4096,
        false,
        [&](const IndexRange range, bool value) {
          if (value) {
            return value;
          }
          shapes_mask.slice(range).foreach_index([&](const int shape_id) {
            if (this->is_in_shape(shape_id, shapes, fill_rule)) {
              value = true;
              return;
            }
          });
          return value;
        },
        std::logical_or());
  }

  bool is_contributing(const CurveBooleanOpParameters op_params,
                       const Vector<IndexMask> &shapes,
                       const int subject_shape,
                       const IndexMask &clipping_shapes) const
  {
    const bool subj = this->is_in_shape(subject_shape, shapes, op_params.subject_rule);
    const bool clip = this->is_in_shapes(clipping_shapes, shapes, op_params.clipping_rule);

    switch (op_params.boolean_mode) {
      case Operation::Intersect: {
        return subj && clip;
      }
      case Operation::Difference: {
        return subj && !clip;
      }
      case Operation::Union: {
        return subj || clip;
      }
      default:
        BLI_assert_unreachable();
        break;
    }

    return false;
  }
};

static std::pair<WindingState, WindingState> LR_states_from_segment(
    const Segment &segment,
    const Span<float2> points,
    const OffsetIndices<int> points_by_curve,
    const Vector<IndexMask> &shapes,
    const IndexMask &mask_shapes,
    const VArray<bool> &is_fill)
{
  WindingState state_L;
  WindingState state_R;

  const int curve_i = segment.curve;

  if (is_fill[curve_i]) {
    if (!segment.is_loop()) {
      const float2 first_point_i = math::interpolate(
          points[segment.start_edge().x], points[segment.start_edge().y], segment.start_alpha());
      const Span<float2> poly_i = points.slice(points_by_curve[curve_i]);
      const int winding_twice_i = point_in_polygon_winding_twice(first_point_i, poly_i);

      /* The point should be exactly on the edge. */
      BLI_assert(math::abs(winding_twice_i) % 2 == 1);
      /* Each state represents a point infinitesimally offset to the left and right. */
      state_L.add_to_curve(curve_i, int((winding_twice_i + 1) / 2));
      state_R.add_to_curve(curve_i, int((winding_twice_i - 1) / 2));
    }
    else {
      state_L.add_to_curve(curve_i, 1);
    }
  }

  float2 first_point = points[segment.start_point()];

  /* If there are no control points in the segment calculate the starting point. */
  if (segment.points_num() == 0) {
    first_point = math::interpolate(
        points[segment.start_edge().x], points[segment.start_edge().y], segment.start_alpha());
  }

  mask_shapes.foreach_index([&](const int shape_id) {
    const IndexMask &curves_j = shapes[shape_id];
    curves_j.foreach_index([&](const int curve_j) {
      if (curve_j == curve_i) {
        return;
      }

      if (is_fill[curve_j]) {
        const Span<float2> poly_j = points.slice(points_by_curve[curve_j]);
        const int winding_j = point_in_polygon_winding_int(first_point, poly_j);
        state_L.add_to_curve(curve_j, winding_j);
        state_R.add_to_curve(curve_j, winding_j);
      }
    });
  });

  return {state_L, state_R};
}

class SegmentEndPoint {
 private:
  int index_ = 0;

 public:
  constexpr SegmentEndPoint() = default;

  constexpr explicit SegmentEndPoint(int segment_i, bool is_end)
  {
    BLI_assert(segment_i >= 0);
    if (is_end) {
      index_ = -(segment_i + 1);
    }
    else {
      index_ = segment_i + 1;
    }
  }

  constexpr friend bool operator==(SegmentEndPoint a, SegmentEndPoint b)
  {
    return a.index_ == b.index_;
  }
  constexpr friend bool operator!=(SegmentEndPoint a, SegmentEndPoint b)
  {
    return !(a == b);
  }

  bool is_start() const
  {
    return index_ > 0;
  }
  bool is_end() const
  {
    return index_ < 0;
  }
  bool is_null() const
  {
    return index_ == 0;
  }

  int segment_index() const
  {
    return math::abs(index_) - 1;
  }
};

struct IntersectionPoint {
  int point_a = -1;
  int point_b = -1;
  float alpha_a = -1.0f;
  float alpha_b = -1.0f;
  int curve_a = -1;
  int curve_b = -1;
  SegmentEndPoint start_a;
  SegmentEndPoint end_a;
  SegmentEndPoint start_b;
  SegmentEndPoint end_b;

  constexpr IntersectionPoint() = default;

  float parameter_for_curve(const int curve) const
  {
    BLI_assert(curve == curve_a || curve == curve_b);
    return curve == curve_a ? point_a + alpha_a : point_b + alpha_b;
  }

  int other_curve(const int curve) const
  {
    BLI_assert(curve == curve_a || curve == curve_b);
    return curve == curve_a ? curve_b : curve_a;
  }
};

static IntersectionPoint create_intersection(const int point_a,
                                             const int point_b,
                                             const float alpha_a,
                                             const float alpha_b,
                                             const int curve_a,
                                             const int curve_b)
{
  IntersectionPoint inter_point;
  inter_point.point_a = point_a;
  inter_point.point_b = point_b;
  inter_point.alpha_a = alpha_a;
  inter_point.alpha_b = alpha_b;
  inter_point.curve_a = curve_a;
  inter_point.curve_b = curve_b;

  return inter_point;
}

/* Will return -1 if there is no next segment. */
static int get_next_segment(const int current_i,
                            const bool current_reversed,
                            const Span<Segment> all_segments,
                            const Span<IntersectionPoint> intersections,
                            const VArray<bool> &is_fill,
                            const Span<bool> all_inside_left,
                            const Span<bool> all_inside_right)
{
  const Segment current_segment = all_segments[current_i];
  if (!(current_reversed ? current_segment.has_start_intersection() :
                           current_segment.has_end_intersection()))
  {
    return -1;
  }

  const int current_end_index = current_reversed ? current_segment.intersection_index[0] :
                                                   current_segment.intersection_index[1];
  const IntersectionPoint &end_int = intersections[current_end_index];
  const SegmentEndPoint endpoint = SegmentEndPoint(current_i, current_reversed);

  const SegmentEndPoint all_ends[4] = {
      end_int.start_a, end_int.end_a, end_int.start_b, end_int.end_b};

  if (!is_fill[current_segment.curve]) {
    return -1;

    for (const int i : IndexRange(2)) {
      const SegmentEndPoint &nex_end = all_ends[i];
      const int seg_i = nex_end.segment_index();

      if (nex_end == endpoint) {
        continue;
      }

      BLI_assert(all_inside_left[seg_i] == all_inside_right[seg_i]);
      if (!all_inside_left[seg_i]) {
        return seg_i;
      }
    }

    return -1;
  }

  for (const int i : IndexRange(4)) {
    const SegmentEndPoint &nex_end = all_ends[i];
    const int seg_i = nex_end.segment_index();

    if (nex_end == endpoint) {
      continue;
    }

    if (all_inside_left[seg_i] ^ all_inside_right[seg_i]) {
      return seg_i;
    }
  }

  return -1;
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

void check_segments(const CurveBooleanOpParameters &op_params,
                    const int curve_k,
                    const bool is_subj,
                    const int subj_shape_id,
                    const Span<float2> points,
                    const Vector<IndexMask> &shapes,
                    const OffsetIndices<int> points_by_curve,
                    const IndexMask &clipping_shapes,
                    const Span<Segment> all_segments,
                    const Span<IndexRange> all_segments_by_curve,
                    const Span<IntersectionPoint> &intersections,
                    const VArray<bool> &is_fill,
                    MutableSpan<bool> all_inside_left,
                    MutableSpan<bool> all_inside_right)
{
  const IndexRange segments = all_segments_by_curve[curve_k];

  if (segments.is_empty()) {
    return;
  }

  const Segment &first_segment = all_segments[segments.first()];
  const IndexMask &mask_shapes = is_subj ? clipping_shapes : shapes[subj_shape_id];
  auto [state_L, state_R] = LR_states_from_segment(
      first_segment, points, points_by_curve, shapes, mask_shapes, is_fill);

  for (const int seg_i : segments) {
    const Segment &this_segment = all_segments[seg_i];

    if (is_fill[curve_k]) {
      all_inside_left[seg_i] = state_L.is_contributing(
          op_params, shapes, subj_shape_id, clipping_shapes);
      all_inside_right[seg_i] = state_R.is_contributing(
          op_params, shapes, subj_shape_id, clipping_shapes);
    }
    else {
      all_inside_left[seg_i] = state_L.is_in_shapes(
          clipping_shapes, shapes, op_params.clipping_rule);
      all_inside_right[seg_i] = state_R.is_in_shapes(
          clipping_shapes, shapes, op_params.clipping_rule);
    }

    if (!this_segment.has_end_intersection()) {
      continue;
    }
    const int int_p_end = this_segment.intersection_index[1];
    const IntersectionPoint &inter_end = intersections[int_p_end];

    const int other_curve_k = inter_end.other_curve(curve_k);

    if (is_fill[other_curve_k]) {
      const int point_k = curve_k == inter_end.curve_a ? inter_end.point_a : inter_end.point_b;
      const int point_other = curve_k != inter_end.curve_a ? inter_end.point_a : inter_end.point_b;
      const float2 &point1 = points[point_k];
      const float2 &point_other1 = points[point_other];
      const float2 &point_other2 = points[(point_other + 1) % points.size()];
      const bool ccw = cross_tri_v2(point1, point_other1, point_other2) > 0.0;

      /* Crossing a line going left to right is incrementing. */
      state_L.add_to_curve(other_curve_k, ccw ? -1 : 1);
      state_R.add_to_curve(other_curve_k, ccw ? -1 : 1);
    }
  }
}

struct BooleanResult {
  Vector<Segment> segments;
  Vector<bool> segment_reversed;
  Vector<int> segment_offsets;
  Vector<bool> cyclic;
  Vector<int> point_offsets;
  Vector<int> shape_ids;

  void append_result(const BooleanResult &other_result, const int shape_id)
  {
    if (other_result.segments.is_empty()) {
      return;
    }

    for (const int i : other_result.segment_offsets.index_range().drop_front(1)) {
      segment_offsets.append(other_result.segment_offsets[i] + segments.size());
    }
    cyclic.extend(other_result.cyclic);
    segment_reversed.extend(other_result.segment_reversed);
    shape_ids.append_n_times(shape_id, other_result.cyclic.size());

    for (const int i : other_result.segments.index_range()) {
      segments.append(std::move(other_result.segments[i]));
    }
  }
};

void find_intersections_between_curves(const Span<float2> points_i,
                                       const Span<float2> points_j,
                                       const int curve_i,
                                       const int curve_j,
                                       const bool cyclic_i,
                                       const bool cyclic_j,
                                       const int point_offset_i,
                                       const int point_offset_j,
                                       Array<Vector<int>> &r_inters_per_curves,
                                       Vector<IntersectionPoint> &r_intersections)
{
  for (const int i : points_i.index_range().drop_back(cyclic_i ? 0 : 1)) {
    for (const int j : points_j.index_range().drop_back(cyclic_j ? 0 : 1)) {
      float alpha_a, alpha_b;
      const int val = intersect(points_i[i],
                                points_i[(i + 1) % points_i.size()],
                                points_j[j],
                                points_j[(j + 1) % points_j.size()],
                                &alpha_a,
                                &alpha_b);
      if (val == ISECT_LINE_LINE_CROSS) {
        r_inters_per_curves[curve_i].append(r_intersections.size());
        r_inters_per_curves[curve_j].append(r_intersections.size());
        r_intersections.append(create_intersection(
            i + point_offset_i, j + point_offset_j, alpha_a, alpha_b, curve_i, curve_j));
      }
      else if (val == ISECT_LINE_LINE_EXACT) {
        /* TODO(@casey-bianco-davis): Properly handle degeneracy. */
        BLI_assert_unreachable();
      }
    }
  }
}

void find_intersections_between_shapes(const Span<float2> points,
                                       const Vector<IndexMask> &shapes,
                                       const IndexMask &shapes_i,
                                       const IndexMask &shapes_j,
                                       const OffsetIndices<int> points_by_curve,
                                       const VArray<bool> &is_fill,
                                       const VArray<bool> &is_cyclic,
                                       const bool self_intersection,
                                       Array<Vector<int>> &r_inters_per_curves,
                                       Vector<IntersectionPoint> &r_intersections)
{
  shapes_i.foreach_index([&](const int shape_i) {
    const IndexMask &curves_i = shapes[shape_i];
    curves_i.foreach_index([&](const int curve_i) {
      const IndexRange points_i = points_by_curve[curve_i];
      const bool cyclic_i = is_cyclic[curve_i] || is_fill[curve_i];

      shapes_j.foreach_index([&](const int shape_j) {
        const IndexMask &curves_j = shapes[shape_j];
        curves_j.foreach_index([&](const int curve_j) {
          if (self_intersection && shape_i >= shape_j) {
            return;
          }

          const IndexRange points_j = points_by_curve[curve_j];
          const bool cyclic_j = is_cyclic[curve_j] || is_fill[curve_j];

          find_intersections_between_curves(points.slice(points_i),
                                            points.slice(points_j),
                                            curve_i,
                                            curve_j,
                                            cyclic_i,
                                            cyclic_j,
                                            points_i.first(),
                                            points_j.first(),
                                            r_inters_per_curves,
                                            r_intersections);
        });
      });
    });
  });
}

bool check_and_join_segments(Segment &first, const Segment &second)
{
  if (first.curve != second.curve) {
    return false;
  }

  if (first.intersection_index[1] == second.intersection_index[0]) {
    first.point_2 = second.point_2;
    first.alpha_2 = second.alpha_2;

    first.intersection_index[1] = second.intersection_index[1];
    return true;
  }
  if (first.intersection_index[0] == second.intersection_index[1]) {
    first.point_1 = second.point_1;
    first.alpha_1 = second.alpha_1;

    first.intersection_index[0] = second.intersection_index[0];
    return true;
  }

  return false;
}

BooleanResult execute_single_boolean(const CurveBooleanOpParameters op_params,
                                     const int subj_shape_id,
                                     const Span<float2> points,
                                     const Vector<IndexMask> &shapes,
                                     const OffsetIndices<int> points_by_curve,
                                     const IndexMask &clipping_shapes,
                                     const Array<Vector<int>> &self_clipping_inters_per_curves,
                                     const Span<IntersectionPoint> clipping_intersections,
                                     const VArray<bool> &is_fill,
                                     const VArray<bool> &is_cyclic)
{
  const IndexMask &curves_i = shapes[subj_shape_id];

  Vector<IntersectionPoint> intersections;
  intersections.extend(clipping_intersections);

  Array<Vector<int>> inters_per_curves(points_by_curve.size());

  find_intersections_between_shapes(points,
                                    shapes,
                                    IndexMask(IndexRange::from_single(subj_shape_id)),
                                    clipping_shapes,
                                    points_by_curve,
                                    is_fill,
                                    is_cyclic,
                                    false,
                                    inters_per_curves,
                                    intersections);

  /* -------------------- */

  Vector<Segment> all_segments;
  Array<IndexRange> all_segments_by_curve(points_by_curve.size());

  auto add_segments = [&](const int curve_k) {
    const IndexRange points_k = points_by_curve[curve_k];
    const Span<int> other_inter = inters_per_curves[curve_k];
    const Span<int> self_inter = self_clipping_inters_per_curves[curve_k];

    const int start_size = all_segments.size();

    if (other_inter.size() == 0 && self_inter.size() == 0) {
      all_segments.append(
          Segment::from_curve(curve_k, points_k, is_cyclic[curve_k] || is_fill[curve_k]));
      all_segments_by_curve[curve_k] = all_segments.index_range().drop_front(start_size);

      return;
    }

    Array<int> new_inters(other_inter.size() + self_inter.size());
    new_inters.as_mutable_span().take_back(other_inter.size()).copy_from(other_inter);
    new_inters.as_mutable_span().take_front(self_inter.size()).copy_from(self_inter);

    Array<int> inter_sorted_ids = Array<int>(new_inters.size());
    array_utils::fill_index_range<int>(inter_sorted_ids);

    parallel_sort(inter_sorted_ids.begin(), inter_sorted_ids.end(), [&](int i1, int i2) {
      const IntersectionPoint &inter1 = intersections[new_inters[i1]];
      const IntersectionPoint &inter2 = intersections[new_inters[i2]];
      return inter1.parameter_for_curve(curve_k) < inter2.parameter_for_curve(curve_k);
    });

    if (is_cyclic[curve_k] || is_fill[curve_k]) {
      const int int_p_1 = new_inters[inter_sorted_ids.first()];
      const int int_p_2 = new_inters[inter_sorted_ids.last()];

      IntersectionPoint &inter_first = intersections[int_p_1];
      IntersectionPoint &inter_last = intersections[int_p_2];

      all_segments.append(Segment::from_intersections(curve_k,
                                                      points_k,
                                                      inter_last.parameter_for_curve(curve_k),
                                                      inter_first.parameter_for_curve(curve_k),
                                                      int_p_2,
                                                      int_p_1));
    }
    else {
      const int int_p_1 = new_inters[inter_sorted_ids.first()];
      IntersectionPoint &inter_first = intersections[int_p_1];

      all_segments.append(Segment::from_start_to_intersection(
          curve_k, points_k, inter_first.parameter_for_curve(curve_k), int_p_1));
    }

    for (const int inter_id : inter_sorted_ids.index_range().drop_back(1)) {
      const int int_p_1 = new_inters[inter_sorted_ids[inter_id]];
      const int int_p_2 = new_inters[inter_sorted_ids[inter_id + 1]];

      IntersectionPoint &inter_first = intersections[int_p_1];
      IntersectionPoint &inter_last = intersections[int_p_2];

      all_segments.append(Segment::from_intersections(curve_k,
                                                      points_k,
                                                      inter_first.parameter_for_curve(curve_k),
                                                      inter_last.parameter_for_curve(curve_k),
                                                      int_p_1,
                                                      int_p_2));
    }

    if (!(is_cyclic[curve_k] || is_fill[curve_k])) {
      const int int_p_2 = new_inters[inter_sorted_ids.last()];
      IntersectionPoint &inter_last = intersections[int_p_2];

      all_segments.append(Segment::from_intersection_to_end(
          curve_k, points_k, inter_last.parameter_for_curve(curve_k), int_p_2));
    }

    all_segments_by_curve[curve_k] = all_segments.index_range().drop_front(start_size);
  };

  /* -------------------- */

  curves_i.foreach_index([&](const int curve_i) { add_segments(curve_i); });
  clipping_shapes.foreach_index([&](const int clip_shape_id) {
    const IndexMask &curves_j = shapes[clip_shape_id];
    curves_j.foreach_index([&](const int curve_j) { add_segments(curve_j); });
  });

  /* -------------------- */

  for (const int seg_i : all_segments.index_range()) {
    const Segment &segment = all_segments[seg_i];
    const int curve_i = segment.curve;

    if (segment.has_start_intersection()) {
      IntersectionPoint &inter_start = intersections[segment.intersection_index[0]];
      if (curve_i == inter_start.curve_a) {
        inter_start.end_a = SegmentEndPoint(seg_i, true);
      }
      else {
        inter_start.end_b = SegmentEndPoint(seg_i, true);
      }
    }

    if (segment.has_end_intersection()) {
      IntersectionPoint &inter_end = intersections[segment.intersection_index[1]];
      if (curve_i == inter_end.curve_a) {
        inter_end.start_a = SegmentEndPoint(seg_i, false);
      }
      else {
        inter_end.start_b = SegmentEndPoint(seg_i, false);
      }
    }
  }

  /* -------------------- */

  Array<bool> all_inside_left(all_segments.size());
  Array<bool> all_inside_right(all_segments.size());

  curves_i.foreach_index([&](const int curve_i) {
    check_segments(op_params,
                   curve_i,
                   true,
                   subj_shape_id,
                   points,
                   shapes,
                   points_by_curve,
                   clipping_shapes,
                   all_segments,
                   all_segments_by_curve,
                   intersections,
                   is_fill,
                   all_inside_left,
                   all_inside_right);
  });
  clipping_shapes.foreach_index([&](const int clip_shape_id) {
    const IndexMask &curves_j = shapes[clip_shape_id];
    curves_j.foreach_index([&](const int curve_j) {
      check_segments(op_params,
                     curve_j,
                     false,
                     subj_shape_id,
                     points,
                     shapes,
                     points_by_curve,
                     clipping_shapes,
                     all_segments,
                     all_segments_by_curve,
                     intersections,
                     is_fill,
                     all_inside_left,
                     all_inside_right);
    });
  });

  /* -------------------- */

  /* Follow each segment until it loops or ends. */
  Array<bool> processed_segments(all_segments.size(), false);

  /* Remove all noncontributing segments. */
  for (const int segment_i : all_segments.index_range()) {
    const Segment &segment = all_segments[segment_i];
    if (is_fill[segment.curve]) {
      if (!all_inside_left[segment_i] ^ all_inside_right[segment_i]) {
        processed_segments[segment_i] = true;
      }
    }
    else {
      BLI_assert(all_inside_left[segment_i] == all_inside_right[segment_i]);
      if (all_inside_left[segment_i]) {
        processed_segments[segment_i] = true;
      }
    }
  }

  int start_segment = processed_segments.as_span().first_index_try(false);

  BooleanResult result;
  result.segment_offsets.append(0);

  while (start_segment != -1) {
    int current_i = start_segment;

    bool PolygonDone = false;
    bool PolygonClosed = false;
    bool last_reversed = false;
    while (!PolygonDone) {
      if (processed_segments[current_i] == true) {
        BLI_assert_unreachable();
        break;
      }

      const Segment &current_segment = all_segments[current_i];
      processed_segments[current_i] = true;

      if (result.segments.size() == 0) {
        result.segments.append(current_segment);
        result.segment_reversed.append(last_reversed);
      }
      /* Check if the last segment can be joined with this one. */
      else if (!check_and_join_segments(result.segments.last(), current_segment)) {
        result.segments.append(current_segment);
        result.segment_reversed.append(last_reversed);
      }

      const int next_segment = get_next_segment(current_i,
                                                last_reversed,
                                                all_segments,
                                                intersections,
                                                is_fill,
                                                all_inside_left,
                                                all_inside_right);

      if (next_segment == -1) {
        PolygonDone = true;
        PolygonClosed = current_segment.is_loop();
        break;
      }

      if (next_segment == start_segment) {
        PolygonDone = true;
        PolygonClosed = true;

        /* Check if the last segment can be joined to the first one. */
        if ((!result.segments.index_range().is_empty()) &&
            result.segment_offsets.last() != result.segments.index_range().last())
        {
          if (check_and_join_segments(result.segments[result.segment_offsets.last()],
                                      result.segments.last()))
          {
            result.segments.remove_last();
          }
        }

        break;
      }

      const int current_end_index = last_reversed ? current_segment.intersection_index[0] :
                                                    current_segment.intersection_index[1];
      const Segment &next_seg = all_segments[next_segment];
      const bool next_reversed = next_seg.intersection_index[0] != current_end_index;

      last_reversed = next_reversed;
      current_i = next_segment;
    }
    result.segment_offsets.append(result.segments.size());
    result.cyclic.append(PolygonClosed);

    /* Get the next unprocessed segment. */
    start_segment = processed_segments.as_span().first_index_try(false);
  }

  return result;
}

static BooleanResult execute_boolean(const CurveBooleanOpParameters op_params,
                                     const Span<float2> points,
                                     const OffsetIndices<int> points_by_curve,
                                     const IndexMask &clipping_shapes,
                                     const IndexMask &mask_shapes,
                                     const bool mask_only,
                                     const VArray<int> &shape_ids,
                                     const VArray<bool> &is_fill,
                                     const VArray<bool> &is_cyclic)
{
  IndexMaskMemory memory;
  VectorSet<int> shape_indexing;
  const Vector<IndexMask> shapes = IndexMask::from_group_ids(shape_ids, memory, shape_indexing);

  Vector<IntersectionPoint> intersections;

  Array<Vector<int>> self_clipping_inters_per_curves(points_by_curve.size());

  find_intersections_between_shapes(points,
                                    shapes,
                                    clipping_shapes,
                                    clipping_shapes,
                                    points_by_curve,
                                    is_fill,
                                    is_cyclic,
                                    true,
                                    self_clipping_inters_per_curves,
                                    intersections);

  BooleanResult results_all;
  results_all.segment_offsets.append(0);

  if (mask_only) {
    const IndexMask subject_shapes = clipping_shapes.complement(mask_shapes, memory);

    subject_shapes.foreach_index([&](const int subj_shape_id) {
      const BooleanResult result = execute_single_boolean(op_params,
                                                          subj_shape_id,
                                                          points,
                                                          shapes,
                                                          points_by_curve,
                                                          clipping_shapes,
                                                          self_clipping_inters_per_curves,
                                                          intersections,
                                                          is_fill,
                                                          is_cyclic);

      results_all.append_result(result, subj_shape_id);
    });
  }
  else {
    const IndexMask subject_shapes = clipping_shapes.complement(shapes.index_range(), memory);

    subject_shapes.foreach_index([&](const int subj_shape_id) {
      if (mask_shapes.contains(subj_shape_id)) {
        const BooleanResult result = execute_single_boolean(op_params,
                                                            subj_shape_id,
                                                            points,
                                                            shapes,
                                                            points_by_curve,
                                                            clipping_shapes,
                                                            self_clipping_inters_per_curves,
                                                            intersections,
                                                            is_fill,
                                                            is_cyclic);

        results_all.append_result(result, subj_shape_id);
      }
      else {
        BooleanResult result;
        result.segment_offsets.append(0);

        shapes[subj_shape_id].foreach_index([&](const int curve_i, const int pos_i) {
          result.segments.append(
              Segment::from_curve(curve_i, points_by_curve[curve_i], is_cyclic[curve_i]));
          result.cyclic.append(is_cyclic[curve_i]);
          result.segment_offsets.append(pos_i + 1);
          result.segment_reversed.append(false);
        });

        results_all.append_result(result, subj_shape_id);
      }
    });
  }

  if (results_all.segments.is_empty()) {
    return results_all;
  }

  results_all.point_offsets.resize(results_all.segment_offsets.size());
  calculate_offsets_from_segments(results_all.segments,
                                  OffsetIndices<int>(results_all.segment_offsets),
                                  results_all.cyclic,
                                  results_all.point_offsets.as_mutable_span());

  return results_all;
}

bke::CurvesGeometry remove_holes(const bke::CurvesGeometry &curves,
                                 const Span<int> shape_ids,
                                 const OffsetIndices<int> points_by_curve)
{
  const VArray<float2> positions_2d_attribute = *curves.attributes().lookup<float2>(
      ".positions_2d", bke::AttrDomain::Point);

  BLI_assert(positions_2d_attribute.is_span());
  const Span<float2> positions_2d = positions_2d_attribute.get_internal_span();

  Vector<int> keep;

  IndexMaskMemory memory;
  VectorSet<int> shape_indexing;
  const Vector<IndexMask> shapes = IndexMask::from_group_ids(
      VArray<int>::ForSpan(shape_ids), memory, shape_indexing);

  for (const int shape_i : shapes.index_range()) {
    const IndexMask shape = shapes[shape_i];
    shape.foreach_index([&](const int curve_i) {
      bool is_inside = false;
      const float2 &point_i = positions_2d[points_by_curve[curve_i].first()];

      shape.foreach_index([&](const int curve_j) {
        if (curve_j == curve_i) {
          return;
        }
        const IndexRange points_j = points_by_curve[curve_j];

        if (inside(point_i, positions_2d.slice(points_j))) {
          is_inside = true;
        }
      });

      if (!is_inside) {
        keep.append(curve_i);
      }
    });
  }

  const IndexMask to_keep = IndexMask::from_indices(keep.as_span(), memory);

  return curves_copy_curve_selection(curves, to_keep, {});
}

bke::CurvesGeometry curve_boolean(const CurveBooleanOpParameters op_params,
                                  const bke::CurvesGeometry &curves,
                                  const IndexMask &mask_shapes,
                                  const IndexMask &clipping_shapes)
{
  const bke::AttributeAccessor src_attributes = curves.attributes();

  const VArray<float2> src_positions_2d_attribute = *src_attributes.lookup<float2>(
      ".positions_2d", bke::AttrDomain::Point);

  BLI_assert(src_positions_2d_attribute.is_span());
  const Span<float2> src_positions_2d = src_positions_2d_attribute.get_internal_span();

  const VArray<bool> is_fills = *src_attributes.lookup<bool>("is_fill", bke::AttrDomain::Curve);
  const VArray<int> shape_ids = *src_attributes.lookup<int>("shape_id", bke::AttrDomain::Curve);

  const BooleanResult result = execute_boolean(op_params,
                                               src_positions_2d,
                                               curves.points_by_curve(),
                                               clipping_shapes,
                                               mask_shapes,
                                               false,
                                               shape_ids,
                                               is_fills,
                                               curves.cyclic());

  if (result.segments.is_empty()) {
    return bke::CurvesGeometry();
  }

  const OffsetIndices<int> dst_segments_by_curve = OffsetIndices<int>(result.segment_offsets);
  const OffsetIndices<int> dst_points_by_curve = OffsetIndices<int>(result.point_offsets);

  if (dst_points_by_curve.total_size() == 0) {
    return bke::CurvesGeometry();
  }

  bke::CurvesGeometry dst_curves(dst_points_by_curve.total_size(), dst_points_by_curve.size());
  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();

  dst_curves.offsets_for_write().copy_from(dst_points_by_curve.data());
  dst_curves.cyclic_for_write().copy_from(result.cyclic);

  bke::SpanAttributeWriter<int> shape_id_writer = dst_attributes.lookup_or_add_for_write_span<int>(
      "shape_id", bke::AttrDomain::Curve);
  shape_id_writer.span.copy_from(result.shape_ids);
  shape_id_writer.finish();

  Array<int> old_by_new_map(dst_points_by_curve.size());

  for (const int i : dst_points_by_curve.index_range()) {
    const IndexRange segment_range = dst_segments_by_curve[i];
    old_by_new_map[i] = result.segments[segment_range.first()].curve;

    /* Find the first segment that is not clipping. */
    for (const int segment_i : segment_range) {
      if (!clipping_shapes.contains(segment_i)) {
        old_by_new_map[i] = result.segments[segment_i].curve;
        break;
      }
    }
  }

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic", "shape_id"}),
                         old_by_new_map,
                         dst_attributes);

  /* Copy/Interpolate point attributes. */
  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes, dst_attributes, ATTR_DOMAIN_MASK_POINT, {}))
  {
    bke::attribute_math::convert_to_static_type(attribute.dst.span.type(), [&](auto dummy) {
      using T = decltype(dummy);
      auto src_attr = attribute.src.typed<T>();
      auto dst_attr = attribute.dst.span.typed<T>();

      int i = 0;

      for (const int curve_i : dst_segments_by_curve.index_range()) {
        const IndexRange segment_range = dst_segments_by_curve[curve_i];
        for (const int seg_i : segment_range) {
          const Segment &segment = result.segments[seg_i];
          const bool reversed = result.segment_reversed[seg_i];

          if (reversed ? segment.has_end_intersection() : segment.has_start_intersection()) {
            const float start_alpha = reversed ? segment.alpha_2 : segment.alpha_1;
            const int2 start_edge = reversed ? segment.end_edge() : segment.start_edge();
            dst_attr[i++] = bke::attribute_math::mix2<T>(
                start_alpha, src_attr[start_edge.x], src_attr[start_edge.y]);
          }

          segment.foreach_point(
              [&](const int index, const int pos) { dst_attr[pos + i] = src_attr[index]; });

          if (reversed) {
            dst_attr.slice(IndexRange::from_begin_size(i, segment.points_num())).reverse();
          }

          i += segment.points_num();

          if (seg_i == segment_range.last() &&
              (reversed ? segment.has_start_intersection() : segment.has_end_intersection()) &&
              !result.cyclic[curve_i])
          {
            const float end_alpha = reversed ? segment.alpha_1 : segment.alpha_2;
            const int2 end_edge = reversed ? segment.start_edge() : segment.end_edge();
            dst_attr[i++] = bke::attribute_math::mix2<T>(
                end_alpha, src_attr[end_edge.x], src_attr[end_edge.y]);
          }
        }
      }
    });

    attribute.dst.finish();
  }

  if (op_params.output_rule == FillRule::NoHoles) {
    dst_curves = remove_holes(dst_curves, result.shape_ids, dst_points_by_curve);
  }

  return dst_curves;
}

}  // namespace blender::geometry::boolean
