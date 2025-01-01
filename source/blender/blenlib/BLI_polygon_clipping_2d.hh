/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

/** \file
 * \ingroup bli
 *
 * This header file contains a C++ interface to the 2D Greiner-Hormann clipping algorithm.
 */

/**
 *
 * TODO
 *
 * Interface for Polygon Clipping in 2D use the Greiner-Hormann clipping algorithm.
 *
 * The input is two lists of positions describing the point in each polygon.
 *
 * Will return `std::nullopt` if the algorithm can not generate valid polygons (i.e has an
 * degeneracies)
 *
 * The output is the following:
 *  1: List of Vertex describing how to interpolate any attribute.
 *  2: Offsets to determent the start and end of each polygon of the output.
 *  3: List of Intersection points.
 *
 */

namespace blender::polygonboolean {

enum class Operation : int8_t {
  /* Intersection of A and B. */
  And,
  /* Union of A and B. */
  Or,
  /* Differences of A with B. */
  Not,
};

struct ExtendedIntersectionPoint {
  int point_a;
  int point_b;
  float alpha_a;
  float alpha_b;
};

static const int NULL_INTERSECTION_ID = -1;
static const int LOOPING_INTERSECTION_ID = -2;

class Segment {
 public:
  int curve;
  IndexRange points;

  int point_1;
  int point_2;

  float alpha_1 = 0.0;
  float alpha_2 = 0.0;

  int inter_index_1 = NULL_INTERSECTION_ID;
  int inter_index_2 = NULL_INTERSECTION_ID;

  bool reversed = false;

  bool is_loop() const
  {
    return inter_index_1 == LOOPING_INTERSECTION_ID;
  }

  int start_intersection() const
  {
    return reversed ? inter_index_2 : inter_index_1;
  }

  int end_intersection() const
  {
    return reversed ? inter_index_1 : inter_index_2;
  }

  bool has_start_intersection() const
  {
    if (this->is_loop()) {
      return false;
    }

    return this->start_intersection() != NULL_INTERSECTION_ID;
  }

  bool has_end_intersection() const
  {
    if (this->is_loop()) {
      return false;
    }

    return this->end_intersection() != NULL_INTERSECTION_ID;
  }

  float start_alpha() const
  {
    return reversed ? alpha_2 : alpha_1;
  }

  float end_alpha() const
  {
    return reversed ? alpha_1 : alpha_2;
  }

  int2 start_edge() const
  {
    if (reversed) {
      return int2(point_2, this->wrap_index(point_2 + 1));
    }
    return int2(point_1, this->wrap_index(point_1 + 1));
  }

  int2 end_edge() const
  {
    if (reversed) {
      return int2(point_1, this->wrap_index(point_1 + 1));
    }
    return int2(point_2, this->wrap_index(point_2 + 1));
  }

  int wrap_index(const int i) const
  {
    return math::mod_periodic(i - points.first(), points.size()) + points.first();
  }

  /*
   * returns the points that are in the segments. The range can go outside of the max and therefor
   * should be wrapped around.
   */
  IndexRange point_range() const
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

  /**
   * Calls the function once for every point.
   *
   * Supported function signatures:
   * - `(int64_t i)`
   * - `(int64_t i, int64_t pos)`
   */
  template<typename Fn> inline void foreach_point(Fn &&fn) const
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

  int points_num() const
  {
    return this->point_range().size();
  }

  constexpr static Segment from_loop(const int curve_i, const IndexRange points)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.points = points;

    segment.point_1 = points.first();
    segment.point_2 = points.last();

    segment.inter_index_1 = LOOPING_INTERSECTION_ID;
    segment.inter_index_2 = LOOPING_INTERSECTION_ID;

    return segment;
  }

  constexpr static Segment from_start_to_end(const int curve_i, const IndexRange points)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.points = points;

    segment.point_1 = points.first();
    segment.point_2 = points.last();

    return segment;
  }

  constexpr static Segment from_intersections(const int curve_i,
                                              const IndexRange points,
                                              const ExtendedIntersectionPoint &inter_first,
                                              const ExtendedIntersectionPoint &inter_last,
                                              const int inter_index_1,
                                              const int inter_index_2)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.points = points;

    segment.point_1 = (curve_i == 0) ? inter_first.point_a : inter_first.point_b;
    segment.alpha_1 = (curve_i == 0) ? inter_first.alpha_a : inter_first.alpha_b;
    segment.inter_index_1 = inter_index_1;

    segment.point_2 = (curve_i == 0) ? inter_last.point_a : inter_last.point_b;
    segment.alpha_2 = (curve_i == 0) ? inter_last.alpha_a : inter_last.alpha_b;
    segment.inter_index_2 = inter_index_2;

    return segment;
  }

  constexpr static Segment from_start_to_intersection(const int curve_i,
                                                      const IndexRange points,
                                                      const ExtendedIntersectionPoint &inter,
                                                      const int inter_index)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.points = points;

    segment.point_1 = points.first();

    segment.point_2 = (curve_i == 0) ? inter.point_a : inter.point_b;
    segment.alpha_2 = (curve_i == 0) ? inter.alpha_a : inter.alpha_b;
    segment.inter_index_2 = inter_index;

    return segment;
  }

  constexpr static Segment from_intersection_to_end(const int curve_i,
                                                    const IndexRange points,
                                                    const ExtendedIntersectionPoint &inter,
                                                    const int inter_index)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.points = points;

    segment.point_1 = (curve_i == 0) ? inter.point_a : inter.point_b;
    segment.alpha_1 = (curve_i == 0) ? inter.alpha_a : inter.alpha_b;
    segment.inter_index_1 = inter_index;

    segment.point_2 = points.last();

    return segment;
  }
};

struct BooleanResult {
  Vector<Segment> segments;
  Vector<int> segment_offsets;
  Vector<bool> cyclic;
  Vector<int> point_offsets;
};

void calculate_positions(Span<float2> pos_a,
                         Span<float2> pos_b,
                         const BooleanResult &result,
                         MutableSpan<float2> dst_pos);

BooleanResult curve_boolean_calc(const Operation boolean_mode,
                                 Span<float2> curve_a,
                                 Span<float2> curve_b,
                                 Span<bool> is_fill,
                                 Span<bool> is_cyclic);

}  // namespace blender::polygonboolean
