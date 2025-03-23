/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "BKE_curves.hh"

/**
 * This header file contains a C++ interface to the 2D boolean clipping, using a modified version
 * of the Greiner-Hormann clipping algorithm.
 */

namespace blender::geometry::boolean {

enum class Operation : int8_t {
  /* Intersection of the Subject and the Clipping. */
  Intersect,
  /* Union of Subject and Clipping. */
  Union,
  /* Differences of Subject with Clipping. */
  Difference,
};

enum class FillRule : int8_t {
  /* Treat odd winding order as fill. */
  EvenOdd,
  /* Treat non zero winding order as fill. */
  NonZero,
  /* Treat non zero winding order as fill but without holes. */
  NoHoles,
};

struct CurveBooleanOpParameters {
  Operation boolean_mode;

  FillRule subject_rule;
  FillRule clipping_rule;
  FillRule output_rule;

  // bool self_intersect; TODO.
};

class Segment {
 public:
  int curve = -1;
  IndexRange points;

  int point_1 = -1;
  int point_2 = -1;

  float alpha_1 = 0.0;
  float alpha_2 = 0.0;

  int inter_index_1 = -1;
  int inter_index_2 = -1;

  bool reversed = false;

  constexpr Segment() = default;

 public:
  bool is_loop() const;

  int start_intersection() const;
  int end_intersection() const;

  bool has_start_intersection() const;
  bool has_end_intersection() const;

  float start_alpha() const;
  float end_alpha() const;

  int2 start_edge() const;
  int2 end_edge() const;

  int start_point() const;
  int end_point() const;

  int wrap_index(const int i) const;

  /*
   * returns the points that are in the segments. The range can go outside of the max and therefor
   * should be wrapped around.
   */
  IndexRange point_range() const;

  /**
   * Calls the function once for every point.
   *
   * Supported function signatures:
   * - `(int64_t i)`
   * - `(int64_t i, int64_t pos)`
   */
  template<typename Fn> inline void foreach_point(Fn &&fn) const;
  int points_num() const;

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
      segment.alpha_1 = 1.0f;
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
    segment.inter_index_1 = inter_index_first;

    segment.point_2 = int(math::floor(parameter_last));
    segment.alpha_2 = math::fract(parameter_last);
    segment.inter_index_2 = inter_index_last;

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
    segment.inter_index_2 = inter_index;

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

    segment.inter_index_1 = inter_index;

    segment.point_2 = points.last();

    return segment;
  }
};

bke::CurvesGeometry curve_boolean(const CurveBooleanOpParameters op_params,
                                  const bke::CurvesGeometry &curves,
                                  const IndexMask &clipping_shapes);

}  // namespace blender::geometry::boolean
