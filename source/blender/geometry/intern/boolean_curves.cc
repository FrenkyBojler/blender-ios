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
#include "BKE_attribute_math.hh"
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
                       const int subject_shape,
                       const IndexMask &clipping_shapes) const
  {
    const bool subj = this->is_in_shape(subject_shape);
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
    const IndexMask &mask_shapes,
    const VArray<bool> &is_fill)
{
  WindingState state_L;
  WindingState state_R;

  state_L.add_to_shape(curve_i, 1); /* TODO. */

  /* TODO: This assumes that the segment size is not zero which is not always true. */
  const int first_point = segment.start_point();

  mask_shapes.foreach_index([&](const int shape_id) {
    /* TODO. */
    const int curve_j = shape_id;

    if (curve_j == curve_i) {
      return;
    }

    if (is_fill[curve_j]) {
      const Span<float2> poly_j = points.slice(points_by_curve[curve_j]);
      int winding_j = point_in_polygon_winding_order(points[first_point], poly_j);
      state_L.add_to_shape(shape_id, winding_j);
      state_R.add_to_shape(shape_id, winding_j);
    }
  });

  return {state_L, state_R};
}

/* Crossing a line going left to right is incrementing. */
// static bool seg_seg_winding(const float2 &P1, const float2 &P2, const float2 &Q1, const float2
// &Q2)
// {
//   /* TODO */
//   return 1;
// }

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

class IntersectionPoint {
 public:
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

/* TODO */
/* Will return -1 if there is no next segment. */
static int get_next_segment(const Span<Segment> unsorted_segments,
                            const int current_segment,
                            const int start_segment,
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

  if (current_segment != start_segment) {
    if (unsorted_segments[start_segment].start_intersection() == current_end_index) {
      *r_reverse_next = unsorted_segments[start_segment].reversed;
      return start_segment;
    }
    if (unsorted_segments[start_segment].end_intersection() == current_end_index) {
      *r_reverse_next = !unsorted_segments[start_segment].reversed;
      return start_segment;
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

struct BooleanResult {
  Vector<Segment> segments;
  Vector<int> segment_offsets;
  Vector<bool> cyclic;
  Vector<int> point_offsets;
};

static BooleanResult execute_boolean(const Operation boolean_mode,
                                     const Span<float2> points,
                                     const OffsetIndices<int> points_by_curve,
                                     const IndexMask &clipping_shapes,
                                     const VArray<bool> &is_fill,
                                     const VArray<bool> &is_cyclic)
{
  /* TODO. */
  IndexMaskMemory memory;
  const IndexMask subject_shapes = clipping_shapes.complement(points_by_curve.index_range(),
                                                              memory);

  Vector<IntersectionPoint> intersections;
  Vector<Segment> unsorted_segments;

  Array<Vector<int>> self_clipping_inters_per_curves(points_by_curve.size());

  clipping_shapes.foreach_index([&](const int shape_i) {
    /* TODO. */
    const int curve_i = shape_i;

    const IndexRange points_i = points_by_curve[curve_i];
    const bool is_cyclic_i = is_cyclic[curve_i] || is_fill[curve_i];

    clipping_shapes.foreach_index([&](const int shape_j) {
      /* TODO. */
      const int curve_j = shape_j;

      if (shape_i >= shape_j) {
        return;
      }

      const IndexRange points_j = points_by_curve[curve_j];
      const bool is_cyclic_j = is_cyclic[curve_j] || is_fill[curve_j];

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
            self_clipping_inters_per_curves[curve_i].append(intersections.size());
            self_clipping_inters_per_curves[curve_j].append(intersections.size());
            intersections.append(
                create_intersection(points_i[i], points_j[j], alpha_a, alpha_b, curve_i, curve_j));
          }
          else if (val == ISECT_LINE_LINE_EXACT) {
            /* TODO */
            // return std::nullopt;
          }
        }
      }
    });
  });

  /* Calculate all intersections. */
  subject_shapes.foreach_index([&](const int subj_shape_id) {
    /* TODO. */
    const int curve_i = subj_shape_id;
    const IndexRange points_i = points_by_curve[curve_i];
    const bool is_cyclic_i = is_cyclic[curve_i] || is_fill[curve_i];

    Array<Vector<int>> inters_per_curves(points_by_curve.size());

    clipping_shapes.foreach_index([&](const int clip_shape_id) {
      /* TODO. */
      const int curve_j = clip_shape_id;

      const IndexRange points_j = points_by_curve[curve_j];
      const bool is_cyclic_j = is_cyclic[curve_j] || is_fill[curve_j];

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
    });

    auto add_segments = [&](int curve_k) {
      const IndexRange points_k = points_by_curve[curve_k];
      Vector<Segment> segments_k;
      const Span<int> other_inter = inters_per_curves[curve_k];
      const Span<int> self_inter = self_clipping_inters_per_curves[curve_k];

      Array<int> new_inters(other_inter.size() + self_inter.size());
      new_inters.as_mutable_span().take_back(other_inter.size()).copy_from(other_inter);
      new_inters.as_mutable_span().take_front(self_inter.size()).copy_from(self_inter);

      if (new_inters.is_empty()) {
        if (is_cyclic[curve_k] || is_fill[curve_k]) {
          segments_k.append(Segment::from_points_cyclical(curve_k, points_k));
        }
        else {
          segments_k.append(Segment::from_points(curve_k, points_k));
        }
      }
      else {
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

          if (curve_k == inter_first.curve_a) {
            inter_first.start_a = SegmentEndPoint(segments_k.size(), true);
          }
          else {
            inter_first.start_b = SegmentEndPoint(segments_k.size(), true);
          }

          if (curve_k == inter_last.curve_a) {
            inter_last.end_a = SegmentEndPoint(segments_k.size(), false);
          }
          else {
            inter_last.end_b = SegmentEndPoint(segments_k.size(), false);
          }

          segments_k.append(Segment::from_intersections(curve_k,
                                                        points_k,
                                                        inter_last.parameter_for_curve(curve_k),
                                                        inter_first.parameter_for_curve(curve_k),
                                                        int_p_2,
                                                        int_p_1));
        }
        else {
          const int int_p_1 = new_inters[inter_sorted_ids.first()];
          IntersectionPoint &inter_first = intersections[int_p_1];

          if (curve_k == inter_first.curve_a) {
            inter_first.start_a = SegmentEndPoint(segments_k.size(), false);
          }
          else {
            inter_first.start_b = SegmentEndPoint(segments_k.size(), false);
          }

          segments_k.append(Segment::from_start_to_intersection(
              curve_k, points_k, inter_first.parameter_for_curve(curve_k), int_p_1));
        }

        for (const int inter_id : inter_sorted_ids.index_range().drop_back(1)) {
          const int int_p_1 = new_inters[inter_sorted_ids[inter_id]];
          const int int_p_2 = new_inters[inter_sorted_ids[inter_id + 1]];

          IntersectionPoint &inter_first = intersections[int_p_1];
          IntersectionPoint &inter_last = intersections[int_p_2];

          if (curve_k == inter_first.curve_a) {
            inter_first.start_a = SegmentEndPoint(segments_k.size(), false);
          }
          else {
            inter_first.start_b = SegmentEndPoint(segments_k.size(), false);
          }

          if (curve_k == inter_last.curve_a) {
            inter_last.end_a = SegmentEndPoint(segments_k.size(), true);
          }
          else {
            inter_last.end_b = SegmentEndPoint(segments_k.size(), true);
          }

          segments_k.append(Segment::from_intersections(curve_k,
                                                        points_k,
                                                        inter_first.parameter_for_curve(curve_k),
                                                        inter_last.parameter_for_curve(curve_k),
                                                        int_p_1,
                                                        int_p_2));
        }

        if (!(is_cyclic[curve_k] || is_fill[curve_k])) {
          const int int_p_2 = new_inters[inter_sorted_ids.last()];
          IntersectionPoint &inter_last = intersections[int_p_2];

          if (curve_k == inter_last.curve_a) {
            inter_last.end_a = SegmentEndPoint(segments_k.size(), true);
          }
          else {
            inter_last.end_b = SegmentEndPoint(segments_k.size(), true);
          }

          segments_k.append(Segment::from_intersection_to_end(
              curve_k, points_k, inter_last.parameter_for_curve(curve_k), int_p_2));
        }
      }

      /* Only add segments that contribute. */
      const IndexRange segments = segments_k.index_range();

      const Segment &first_segment = segments_k[segments.first()];
      const IndexMask &mask_shapes = curve_k == curve_i ? clipping_shapes : subject_shapes;
      auto [state_L, state_R] = LR_states_from_segment(
          first_segment, curve_k, points, points_by_curve, mask_shapes, is_fill);

      for (const int seg_i : segments) {
        const Segment &this_segment = segments_k[seg_i];
        if (state_L.is_contributing(boolean_mode, subj_shape_id, clipping_shapes) ^
            state_R.is_contributing(boolean_mode, subj_shape_id, clipping_shapes))
        {
          unsorted_segments.append(this_segment);
        }

        if (!this_segment.has_end_intersection()) {
          continue;
        }
        const int int_p_end = this_segment.end_intersection();
        const IntersectionPoint &inter_end = intersections[int_p_end];

        const int other_curve_k = inter_end.other_curve(curve_k);
        const int other_shape = other_curve_k; /* TODO. */

        if (is_fill[other_curve_k]) {
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
    };

    add_segments(curve_i);
    clipping_shapes.foreach_index([&](const int clip_shape_id) {
      /* TODO. */
      const int curve_j = clip_shape_id;
      add_segments(curve_j);
    });
  });

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
          unsorted_segments, current_segment, start_segment, processed_segments, &next_reversed);

      if (next_segment == -1) {
        PolygonDone = true;
        PolygonClosed = unsorted_segments[current_segment].is_loop();
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

bke::CurvesGeometry curve_boolean(const Operation boolean_mode,
                                  const bke::CurvesGeometry &curves,
                                  const IndexMask &clipping_shapes)
{
  const bke::AttributeAccessor src_attributes = curves.attributes();

  const VArray<float2> positions_2d_attribute = *src_attributes.lookup<float2>(
      ".positions_2d", bke::AttrDomain::Point);

  BLI_assert(positions_2d_attribute.is_span());
  const Span<float2> positions_2d = positions_2d_attribute.get_internal_span();

  const VArray<bool> is_fills = *src_attributes.lookup<bool>("is_fill", bke::AttrDomain::Curve);
  const BooleanResult result = execute_boolean(boolean_mode,
                                               positions_2d,
                                               curves.points_by_curve(),
                                               clipping_shapes,
                                               is_fills,
                                               curves.cyclic());

  const OffsetIndices<int> dst_segments_by_curve = OffsetIndices<int>(result.segment_offsets);
  const OffsetIndices<int> dst_points_by_curve = OffsetIndices<int>(result.point_offsets);

  bke::CurvesGeometry dst_curves(dst_points_by_curve.total_size(), dst_points_by_curve.size());
  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();

  dst_curves.offsets_for_write().copy_from(dst_points_by_curve.data());
  dst_curves.cyclic_for_write().copy_from(result.cyclic);

  Array<int> old_by_new_map(dst_points_by_curve.size());

  for (const int i : dst_points_by_curve.index_range()) {
    const IndexRange segment_range = dst_segments_by_curve[i];
    old_by_new_map[i] = result.segments[segment_range.first()].curve;

    /* Find the first segment that is not clipping. */
    for (const int segment_i : segment_range) {
      if (!clipping_shapes.contains(segment_i)) {
        old_by_new_map[i] = result.segments[segment_i].curve;
        continue;
      }
    }
  }

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic"}),
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

      /* TODO. */
      for (const int curve_i : dst_segments_by_curve.index_range()) {
        const IndexRange segment_range = dst_segments_by_curve[curve_i];
        for (const int seg_i : segment_range) {
          const Segment &segment = result.segments[seg_i];

          if (segment.has_start_intersection()) {
            dst_attr[i++] = bke::attribute_math::mix2<T>(segment.start_alpha(),
                                                         src_attr[segment.start_edge().x],
                                                         src_attr[segment.start_edge().y]);
          }

          segment.foreach_point(
              [&](const int index, const int pos) { dst_attr[pos + i] = src_attr[index]; });

          i += segment.points_num();

          if (seg_i == segment_range.last() && segment.has_end_intersection() &&
              !result.cyclic[curve_i])
          {
            dst_attr[i++] = bke::attribute_math::mix2<T>(segment.end_alpha(),
                                                         src_attr[segment.end_edge().x],
                                                         src_attr[segment.end_edge().y]);
          }
        }
      }
    });

    attribute.dst.finish();
  }

  return dst_curves;
}

}  // namespace blender::geometry::boolean
