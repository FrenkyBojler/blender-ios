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
    case Operation::Intersect: {
      return !is_fill;
    }
    case Operation::Difference: {
      return !is_fill ^ is_subj;
    }
    case Operation::Union: {
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

void calculate_positions(const Span<float2> pos_subj,
                         const Span<float2> pos_clip,
                         const BooleanResult &result,
                         MutableSpan<float2> dst_pos)
{
  const OffsetIndices<int> segments_by_polygon = OffsetIndices<int>(result.segment_offsets);
  int i = 0;

  for (const int curve_i : segments_by_polygon.index_range()) {
    const IndexRange segment_range = segments_by_polygon[curve_i];
    for (const int seg_i : segment_range) {
      const Segment &segment = result.segments[seg_i];

      const Span<float2> current_curve = (segment.curve == 0) ? pos_subj : pos_clip;

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

BooleanResult execute_boolean(const Operation boolean_mode,
                              const Span<float2> curve_subj,
                              const Span<float2> curve_clip,
                              const Span<bool> is_fill,
                              const Span<bool> is_cyclic)
{
  const int num_curves = 2;
  Array<IndexRange> points_per_curve({curve_subj.index_range(), curve_clip.index_range()});

  Vector<ExtendedIntersectionPoint> intersections;
  Array<Vector<int>> inters_per_curves(num_curves);

  /* Calculate all intersections. */
  {
    const int curve_i = 0;
    const int curve_j = 1;

    const IndexRange points_i = points_per_curve[curve_i];
    const IndexRange points_j = points_per_curve[curve_j];

    const bool is_cyclic_i = is_cyclic[curve_i];
    const bool is_cyclic_j = is_cyclic[curve_j];

    for (const int i : points_i.index_range().drop_back(is_cyclic_i ? 0 : 1)) {
      for (const int j : points_j.index_range().drop_back(is_cyclic_j ? 0 : 1)) {
        float alpha_a, alpha_b;
        const int val = intersect(curve_subj[points_i[i]],
                                  curve_subj[points_i[(i + 1) % points_i.size()]],
                                  curve_clip[points_j[j]],
                                  curve_clip[points_j[(j + 1) % points_j.size()]],
                                  &alpha_a,
                                  &alpha_b);
        if (val == ISECT_LINE_LINE_CROSS) {
          inters_per_curves[curve_i].append(intersections.size());
          inters_per_curves[curve_j].append(intersections.size());
          intersections.append(create_intersection(i, j, alpha_a, alpha_b));
        }
        else if (val == ISECT_LINE_LINE_EXACT) {
          /* TODO */
          // return std::nullopt;
        }
      }
    }
  }

  /* Create all segments that are not internal. */
  Vector<Segment> unsorted_segments;
  for (const int curve_i : IndexRange(num_curves)) {
    const IndexRange points = points_per_curve[curve_i];
    const Vector<int> &inters_per_curve = inters_per_curves[curve_i];

    const bool is_subj = curve_i == 0;

    int current_winding_order = 0;

    /* TODO */
    const Span<float2> poly_this = (curve_i == 0) ? curve_subj : curve_clip;
    const Span<float2> poly_other = (curve_i == 0) ? curve_clip : curve_subj;

    current_winding_order = point_in_polygon_winding_order(poly_this.first(), poly_other);

    if (inters_per_curve.is_empty() &&
        contributing_rule(current_winding_order, is_subj, boolean_mode))
    {
      if (is_cyclic[curve_i]) {
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

    if (is_cyclic[curve_i]) {
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
        const int int_p_1 = inters_per_curve[inter_sorted_ids.first()];
        const ExtendedIntersectionPoint &inter_first = intersections[int_p_1];

        unsorted_segments.append(
            Segment::from_start_to_intersection(curve_i, points, inter_first, int_p_1));
      }
    }

    for (const int inter_id : inter_sorted_ids.index_range().drop_back(1)) {
      const int int_p_1 = inters_per_curve[inter_sorted_ids[inter_id]];
      const int int_p_2 = inters_per_curve[inter_sorted_ids[inter_id + 1]];

      const ExtendedIntersectionPoint &inter_first = intersections[int_p_1];

      const int other_curve_i = 1 - curve_i; /* TODO */

      if (is_fill[other_curve_i]) {
        current_winding_order++; /* TODO */
        // current_winding_order += seg_seg_winding(
        //     curve_subj[inter_first.point_a],
        //     curve_subj[(inter_first.point_a + 1) % curve_subj.size()],
        //     curve_clip[inter_first.point_b],
        //     curve_clip[(inter_first.point_b + 1) % curve_clip.size()]);
      }

      if (contributing_rule(current_winding_order, is_subj, boolean_mode)) {
        const ExtendedIntersectionPoint &inter_last = intersections[int_p_2];

        unsorted_segments.append(Segment::from_intersections(
            curve_i, points, inter_first, inter_last, int_p_1, int_p_2));
      }
    }

    current_winding_order++; /* TODO */
    // if (is_fill[other_curve_i]) {
    //     current_winding_order++; /* TODO */
    //     // current_winding_order += seg_seg_winding(
    //     //     curve_subj[inter_first.point_a],
    //     //     curve_subj[(inter_first.point_a + 1) % curve_subj.size()],
    //     //     curve_clip[inter_first.point_b],
    //     //     curve_clip[(inter_first.point_b + 1) % curve_clip.size()]);
    //   }

    if (!is_cyclic[curve_i] && contributing_rule(current_winding_order, is_subj, boolean_mode)) {
      const int int_p_2 = inter_sorted_ids[inters_per_curve.last()];
      const ExtendedIntersectionPoint &inter_last = intersections[int_p_2];

      unsorted_segments.append(
          Segment::from_intersection_to_end(curve_i, points, inter_last, int_p_2));
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
                                 const Span<float2> curve_subj,
                                 const Span<float2> curve_clip,
                                 const Span<bool> is_fill,
                                 const Span<bool> is_cyclic)
{
  return execute_boolean(boolean_mode, curve_subj, curve_clip, is_fill, is_cyclic);
}

}  // namespace blender::polygonboolean
