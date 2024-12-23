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

enum class VertexType : int8_t {
  PointA,
  PointB,
  Intersection,
};

/**
 * `type` determent which array the `point_id` refers to.
 */
struct Vertex {
  VertexType type;
  int point_id;
};

struct IntersectionPoint {
  int point_a;
  int point_b;
  /**
   * `alpha_a` is the factor between point_a and point_a + 1 (i.e. the next point)
   * And the same is true for `B`
   */
  float alpha_a;
  float alpha_b;
};

struct BooleanResult {
  Array<Vertex> verts;
  Array<int> offsets;
  Array<IntersectionPoint> intersections_data;
};

void interpolate_position_ab(Span<float2> pos_a,
                             Span<float2> pos_b,
                             const BooleanResult &result,
                             MutableSpan<float2> dst_pos);
void interpolate_position_a(const Span<float2> pos_a,
                            const BooleanResult &result,
                            MutableSpan<float2> dst_pos);

std::optional<BooleanResult> curve_boolean_calc(const Operation boolean_mode,
                                                Span<float2> curve_a,
                                                Span<float2> curve_b);
/**
 * `Cut` behaves like `NotB` but with `A` not having any fill, and so `A` is cut into separate
 * parts without any segments of `B` is left in the result.
 */
std::optional<BooleanResult> curve_boolean_cut(const bool is_a_cyclic,
                                               Span<float2> curve_a,
                                               Span<float2> curve_b);

BooleanResult result_remove_holes(const BooleanResult &in_result,
                                  Span<float2> curve_a,
                                  Span<float2> curve_b);
BooleanResult result_sort_holes(const BooleanResult &in_result,
                                Span<float2> curve_a,
                                Span<float2> curve_b);

/**
 * This returns the most appropriate result when the inputted geometry has an degeneracies.
 */
BooleanResult invalid_result(const Operation mode, Span<float2> curve_a, Span<float2> curve_b);

}  // namespace blender::polygonboolean
