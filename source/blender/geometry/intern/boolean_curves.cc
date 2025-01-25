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
 * polygon.
 *
 * This implementation also works in three phases:
 *  1: Break all polygons into segments.
 *  2: Remove all segments that are not contributing.
 *  3: Create polygons by following each segment until it loops or ends.
 *
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
#include "BKE_curves.hh"

#include "GEO_boolean_curves.hh"

namespace blender::geometry::boolean {

bool Segment::is_loop() const
{
  return inter_index_1 == LOOPING_INTERSECTION_ID;
}

int Segment::start_intersection() const
{
  return reversed ? inter_index_2 : inter_index_1;
}

int Segment::end_intersection() const
{
  return reversed ? inter_index_1 : inter_index_2;
}

bool Segment::has_start_intersection() const
{
  if (this->is_loop()) {
    return false;
  }

  return this->start_intersection() != NULL_INTERSECTION_ID;
}

bool Segment::has_end_intersection() const
{
  if (this->is_loop()) {
    return false;
  }

  return this->end_intersection() != NULL_INTERSECTION_ID;
}

float Segment::start_alpha() const
{
  return reversed ? alpha_2 : alpha_1;
}

float Segment::end_alpha() const
{
  return reversed ? alpha_1 : alpha_2;
}

int2 Segment::start_edge() const
{
  if (reversed) {
    return int2(point_2, this->wrap_index(point_2 + 1));
  }
  return int2(point_1, this->wrap_index(point_1 + 1));
}

int2 Segment::end_edge() const
{
  if (reversed) {
    return int2(point_1, this->wrap_index(point_1 + 1));
  }
  return int2(point_2, this->wrap_index(point_2 + 1));
}

int Segment::start_point() const
{
  if (!this->has_start_intersection()) {
    if (reversed) {
      return points.last();
    }
    return points.first();
  }
  return this->start_edge().y;
}

int Segment::end_point() const
{
  return this->end_edge().x;
}

int Segment::wrap_index(const int i) const
{
  return math::mod_periodic(i - points.first(), points.size()) + points.first();
}

IndexRange Segment::point_range() const
{
  if (this->is_loop()) {
    return points;
  }

  if (!this->has_start_intersection() && this->has_end_intersection()) {
    return IndexRange::from_begin_end_inclusive(points.first(), point_2);
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

template<typename Fn> inline void Segment::foreach_point(Fn &&fn) const
{
  const IndexRange point_range = this->point_range();

  for (const int64_t pos : point_range.index_range()) {
    const int i = this->wrap_index(point_range[reversed ? (point_range.size() - 1) - pos : pos]);

    if constexpr (std::is_invocable_r_v<void, Fn, int64_t, int64_t>) {
      fn(i, pos);
    }
    else {
      fn(i);
    }
  }
}

int Segment::points_num() const
{
  return this->point_range().size();
}

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

static int point_in_polygon_winding_order(const float2 &point, const Span<float2> poly)
{
  /* TODO */
  return int(inside(point, poly));
}

class WindingState {
 private:
  /* Winding order of each shape. */
  Map<int, int> orders_;

 public:
  void add_to_shape(const int shape_id, const int winding_i)
  {
    if (winding_i == 0) {
      return;
    }

    if (orders_.contains(shape_id)) {
      orders_.lookup(shape_id) += winding_i;

      /* Remove unneeded ids. */
      if (orders_.lookup(shape_id) == 0) {
        orders_.remove(shape_id);
      }
    }
    else {
      orders_.add(shape_id, winding_i);
    }
  }

  bool is_in_shape(const int shape_id) const
  {
    if (!orders_.contains(shape_id)) {
      return false;
    }

    if (true) { /* TODO. */
      return orders_.lookup(shape_id) % 2 != 0;
    }
    else {
      return orders_.lookup(shape_id) != 0;
    }
  }

  bool is_in_shapes(const IndexMask &shapes) const
  {
    if (orders_.is_empty() || shapes.is_empty()) {
      return false;
    }

    return threading::parallel_reduce(
        shapes.index_range(),
        4096,
        false,
        [&](const IndexRange range, bool value) {
          if (value) {
            return value;
          }
          shapes.slice(range).foreach_index([&](const int shape_id) {
            if (this->is_in_shape(shape_id)) {
              value = true;
              return;
            }
          });
          return value;
        },
        std::logical_or());
  }

  bool is_contributing(const Operation boolean_mode,
                       const IndexRange subject_shapes,
                       const IndexRange clipping_shapes) const
  {
    const bool subj = this->is_in_shapes(subject_shapes);
    const bool clip = this->is_in_shapes(clipping_shapes);

    switch (boolean_mode) {
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

static WindingState state_from_point(const float2 point,
                                     const Span<float2> points,
                                     const OffsetIndices<int> points_by_curve,
                                     const Span<bool> is_fill)
{
  WindingState state;

  for (const int curve_i : points_by_curve.index_range()) {
    const int shape_id = curve_i; /* TODO. */

    if (is_fill[curve_i]) {
      const Span<float2> poly_i = points.slice(points_by_curve[curve_i]);
      state.add_to_shape(shape_id, point_in_polygon_winding_order(point, poly_i));
    }
  }

  return state;
}

static std::pair<WindingState, WindingState> LR_states_from_segment(
    const Segment &segment,
    const int curve_i,
    const Span<float2> points,
    const OffsetIndices<int> points_by_curve,
    const VArray<bool> &is_fill)
{
  WindingState state_L;
  WindingState state_R;

  state_L.add_to_shape(curve_i, 1); /* TODO. */

  /* TODO: This assumes that the segment size is not zero which is not always true. */
  const int first_point = segment.start_point();

  for (const int curve_j : points_by_curve.index_range()) {
    if (curve_j == curve_i) {
      continue;
    }

    const int shape_id = curve_j; /* TODO. */

    if (is_fill[curve_j]) {
      const Span<float2> poly_j = points.slice(points_by_curve[curve_j]);
      state_L.add_to_shape(shape_id, point_in_polygon_winding_order(points[first_point], poly_j));
      state_R.add_to_shape(shape_id, point_in_polygon_winding_order(points[first_point], poly_j));
    }
  }

  return {state_L, state_R};
}

/* Crossing a line going left to right is incrementing. */
// static bool seg_seg_winding(const float2 &P1, const float2 &P2, const float2 &Q1, const float2
// &Q2)
// {
//   /* TODO */
//   return 1;
// }

struct ExtendedIntersectionPoint {
  int point_a;
  int point_b;
  float alpha_a;
  float alpha_b;
  int curve_a;
  int curve_b;

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

static ExtendedIntersectionPoint create_intersection(const int point_a,
                                                     const int point_b,
                                                     const float alpha_a,
                                                     const float alpha_b,
                                                     const int curve_a,
                                                     const int curve_b)
{
  ExtendedIntersectionPoint inter_point;
  inter_point.point_a = point_a;
  inter_point.point_b = point_b;
  inter_point.alpha_a = alpha_a;
  inter_point.alpha_b = alpha_b;
  inter_point.curve_a = curve_a;
  inter_point.curve_b = curve_b;

  return inter_point;
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

void calculate_positions(const Span<float2> points,
                         const BooleanResult &result,
                         MutableSpan<float2> dst_pos)
{
  const OffsetIndices<int> segments_by_polygon = OffsetIndices<int>(result.segment_offsets);
  int i = 0;

  for (const int curve_i : segments_by_polygon.index_range()) {
    const IndexRange segment_range = segments_by_polygon[curve_i];
    for (const int seg_i : segment_range) {
      const Segment &segment = result.segments[seg_i];

      if (segment.has_start_intersection()) {
        dst_pos[i++] = math::interpolate(
            points[segment.start_edge().x], points[segment.start_edge().y], segment.start_alpha());
      }

      segment.foreach_point(
          [&](const int index, const int pos) { dst_pos[pos + i] = points[index]; });

      i += segment.points_num();

      if (seg_i == segment_range.last() && segment.has_end_intersection() &&
          !result.cyclic[curve_i])
      {
        dst_pos[i++] = math::interpolate(
            points[segment.end_edge().x], points[segment.end_edge().y], segment.end_alpha());
      }
    }
  }
}

BooleanResult execute_boolean(const Operation boolean_mode,
                              const Span<float2> points,
                              const OffsetIndices<int> points_by_curve,
                              const IndexRange clipping_shapes,
                              const VArray<bool> &is_fill,
                              const VArray<bool> &is_cyclic)
{
  Vector<ExtendedIntersectionPoint> intersections;
  Array<Vector<int>> inters_per_curves(points_by_curve.size());

  /* Calculate all intersections. */
  for (const int curve_i : points_by_curve.index_range()) {
    const IndexRange points_i = points_by_curve[curve_i];
    const bool is_cyclic_i = is_cyclic[curve_i];

    for (const int curve_j : points_by_curve.index_range()) {
      if (curve_i == curve_j) {
        continue;
      }
      if (curve_i > curve_j) {
        continue;
      }

      const IndexRange points_j = points_by_curve[curve_j];
      const bool is_cyclic_j = is_cyclic[curve_j];

      for (const int i : points_i.index_range().drop_back(is_cyclic_i ? 0 : 1)) {
        for (const int j : points_j.index_range().drop_back(is_cyclic_j ? 0 : 1)) {
          float alpha_a, alpha_b;
          const int val = intersect(points[points_i[i]],
                                    points[points_i[(i + 1) % points_i.size()]],
                                    points[points_j[j]],
                                    points[points_j[(j + 1) % points_j.size()]],
                                    &alpha_a,
                                    &alpha_b);
          if (val == ISECT_LINE_LINE_CROSS) {
            inters_per_curves[curve_i].append(intersections.size());
            inters_per_curves[curve_j].append(intersections.size());
            intersections.append(
                create_intersection(points_i[i], points_j[j], alpha_a, alpha_b, curve_i, curve_j));
          }
          else if (val == ISECT_LINE_LINE_EXACT) {
            /* TODO */
            // return std::nullopt;
          }
        }
      }
    }
  }

  /* Create all segments. */
  Vector<Segment> all_segments;
  Vector<int> all_segment_offsets;
  for (const int curve_i : points_by_curve.index_range()) {
    const IndexRange points_i = points_by_curve[curve_i];
    const Vector<int> &inters_per_curve = inters_per_curves[curve_i];
    all_segment_offsets.append(all_segments.size());

    if (inters_per_curve.is_empty()) {
      if (is_cyclic[curve_i]) {
        all_segments.append(Segment::from_points_cyclical(curve_i, points_i));
      }
      else {
        all_segments.append(Segment::from_points(curve_i, points_i));
      }
      continue;
    }

    Array<int> inter_sorted_ids = Array<int>(inters_per_curve.size());
    array_utils::fill_index_range<int>(inter_sorted_ids);

    parallel_sort(inter_sorted_ids.begin(), inter_sorted_ids.end(), [&](int i1, int i2) {
      const ExtendedIntersectionPoint &inter1 = intersections[inters_per_curve[i1]];
      const ExtendedIntersectionPoint &inter2 = intersections[inters_per_curve[i2]];
      return inter1.parameter_for_curve(curve_i) < inter2.parameter_for_curve(curve_i);
    });

    if (is_cyclic[curve_i]) {
      const int int_p_1 = inters_per_curve[inter_sorted_ids.first()];
      const int int_p_2 = inters_per_curve[inter_sorted_ids.last()];

      const ExtendedIntersectionPoint &inter_first = intersections[int_p_1];
      const ExtendedIntersectionPoint &inter_last = intersections[int_p_2];

      all_segments.append(Segment::from_intersections(curve_i,
                                                      points_i,
                                                      inter_last.parameter_for_curve(curve_i),
                                                      inter_first.parameter_for_curve(curve_i),
                                                      int_p_2,
                                                      int_p_1));
    }
    else {
      const int int_p_1 = inters_per_curve[inter_sorted_ids.first()];
      const ExtendedIntersectionPoint &inter_first = intersections[int_p_1];

      all_segments.append(Segment::from_start_to_intersection(
          curve_i, points_i, inter_first.parameter_for_curve(curve_i), int_p_1));
    }

    for (const int inter_id : inter_sorted_ids.index_range().drop_back(1)) {
      const int int_p_1 = inters_per_curve[inter_sorted_ids[inter_id]];
      const int int_p_2 = inters_per_curve[inter_sorted_ids[inter_id + 1]];

      const ExtendedIntersectionPoint &inter_first = intersections[int_p_1];
      const ExtendedIntersectionPoint &inter_last = intersections[int_p_2];

      all_segments.append(Segment::from_intersections(curve_i,
                                                      points_i,
                                                      inter_first.parameter_for_curve(curve_i),
                                                      inter_last.parameter_for_curve(curve_i),
                                                      int_p_1,
                                                      int_p_2));
    }

    if (!is_cyclic[curve_i]) {
      const int int_p_2 = inter_sorted_ids[inters_per_curve.last()];
      const ExtendedIntersectionPoint &inter_last = intersections[int_p_2];

      all_segments.append(Segment::from_intersection_to_end(
          curve_i, points_i, inter_last.parameter_for_curve(curve_i), int_p_2));
    }
  }
  all_segment_offsets.append(all_segments.size());

  /* -------------------- */

  /* TODO. */
  const IndexRange subject_shapes = IndexRange::from_begin_end(0, clipping_shapes.first());

  const OffsetIndices<int> all_segments_by_curve = OffsetIndices<int>(all_segment_offsets);
  Vector<Segment> unsorted_segments;

  /* Remove all segments that don't contribute. */
  for (const int curve_i : all_segments_by_curve.index_range()) {
    const IndexRange segments = all_segments_by_curve[curve_i];

    const Segment &first_segment = all_segments[segments.first()];
    auto [state_L, state_R] = LR_states_from_segment(
        first_segment, curve_i, points, points_by_curve, is_fill);

    for (const int seg_i : segments) {
      const Segment &this_segment = all_segments[seg_i];
      if (state_L.is_contributing(boolean_mode, subject_shapes, clipping_shapes) ^
          state_R.is_contributing(boolean_mode, subject_shapes, clipping_shapes))
      {
        unsorted_segments.append(this_segment);
      }

      if (!this_segment.has_end_intersection()) {
        continue;
      }
      const int int_p_end = this_segment.end_intersection();
      const ExtendedIntersectionPoint &inter_end = intersections[int_p_end];

      const int other_curve_i = inter_end.other_curve(curve_i);
      const int other_shape = other_curve_i; /* TODO. */

      if (is_fill[other_curve_i]) {
        /* TODO */
        state_L.add_to_shape(other_shape, 1);
        state_R.add_to_shape(other_shape, 1);
        // current_winding_order += seg_seg_winding(
        //     curve_subj[inter_first.point_a],
        //     curve_subj[(inter_first.point_a + 1) % curve_subj.size()],
        //     curve_clip[inter_first.point_b],
        //     curve_clip[(inter_first.point_b + 1) % curve_clip.size()]);
      }
    }
  }

  /* -------------------- */

  if (unsorted_segments.is_empty()) {
    BooleanResult result;
    return result;
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
        break;
      }

      unsorted_segments[current_segment].reversed = last_reversed;
      result.segments.append(unsorted_segments[current_segment]);
      processed_segments[current_segment] = true;
      result.segments.last().reversed = last_reversed;

      bool next_reversed;
      const int next_segment = get_next_segment(
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

BooleanResult curve_boolean_calc(const Operation boolean_mode,
                                 const bke::CurvesGeometry &curves,
                                 const Span<float2> positions_2d,
                                 const IndexRange clipping_shapes)
{
  const bke::AttributeAccessor attributes = curves.attributes();

  const VArray<bool> is_fills = *attributes.lookup<bool>("is_fill", bke::AttrDomain::Curve);
  return execute_boolean(boolean_mode,
                         positions_2d,
                         curves.points_by_curve(),
                         clipping_shapes,
                         is_fills,
                         curves.cyclic());
}

}  // namespace blender::geometry::boolean
