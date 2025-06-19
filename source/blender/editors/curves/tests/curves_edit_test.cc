/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <algorithm>

#include "ED_curves.hh"

#include "../intern/nurbs_intern.hh"
#include "testing/testing.h"

namespace blender::ed::curves::tests {

static bke::CurvesGeometry create_curves(const Span<Vector<float3>> all_positions,
                                         const int order,
                                         const Set<int> &is_cyclic)
{
  Array<int> offsets(all_positions.size() + 1, 0);
  for (const int curve : all_positions.index_range()) {
    const Vector<float3> &curve_positions = all_positions[curve];
    offsets[curve + 1] = offsets[curve] + curve_positions.size();
  }

  bke::CurvesGeometry curves(offsets.last(), all_positions.size());

  curves.offsets_for_write().copy_from(offsets);
  const OffsetIndices points_by_curve = curves.points_by_curve();
  MutableSpan<float3> positions = curves.positions_for_write();
  MutableSpan<bool> cyclic = curves.cyclic_for_write();
  MutableSpan<int8_t> orders = curves.nurbs_orders_for_write();

  for (const int curve : all_positions.index_range()) {
    positions.slice(points_by_curve[curve]).copy_from(all_positions[curve]);
    cyclic[curve] = is_cyclic.contains(curve);
    orders[curve] = order;
  }

  curves.tag_topology_changed();
  return curves;
}

static bke::CurvesGeometry create_curves(const Vector<float3> positions,
                                         const int order,
                                         const Set<int> &is_cyclic)
{
  return create_curves(Span<Vector<float3>>(&positions, 1), order, is_cyclic);
}

static void validate_positions(const Span<Vector<float3>> expected_positions,
                               const OffsetIndices<int> points_by_curve,
                               const Span<float3> positions)
{
  for (const int curve : expected_positions.index_range()) {
    const Span<float3> expected_curve_positions = expected_positions[curve];
    const IndexRange points = points_by_curve[curve];
    for (const int point : expected_curve_positions.index_range()) {
      EXPECT_EQ(positions[points[point]], expected_curve_positions[point]);
    }
  }
}

TEST(curves_editors, DuplicatePointsTwoSingle)
{
  /* Two points from single curve. */
  const Vector<float3> expected_positions = {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}};

  bke::CurvesGeometry curves = create_curves(expected_positions, 4, {});
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(Array<int>{1, 2}.as_span(), memory);

  duplicate_points(curves, mask);

  EXPECT_TRUE(curves.curves_num() == 2);

  const Span<float3> positions = curves.positions();

  for (const int point : expected_positions.index_range()) {
    EXPECT_TRUE(positions[point] == expected_positions[point]);
  }

  EXPECT_TRUE(positions[4] == expected_positions[1]);
  EXPECT_TRUE(positions[5] == expected_positions[2]);
}

TEST(curves_editors, DuplicatePointsFourThree)
{
  /* Four points from three curves. One curve has one point. */
  const Vector<Vector<float3>> expected_positions = {
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}},
      {{0, 0, 0}},
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}, {1, -1, 0}}};

  bke::CurvesGeometry curves = create_curves(expected_positions, 4, {});
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(Array<int>{0, 1, 4, 9}.as_span(), memory);

  duplicate_points(curves, mask);

  EXPECT_TRUE(curves.curves_num() == expected_positions.size() + 3);

  const Span<float3> positions = curves.positions();
  const OffsetIndices points_by_curve = curves.points_by_curve();

  for (const int curve : expected_positions.index_range()) {
    const Span<float3> expected_curve_positions = expected_positions[curve];
    const IndexRange points = points_by_curve[curve];
    for (const int point : expected_curve_positions.index_range()) {
      EXPECT_TRUE(positions[points[point]] == expected_curve_positions[point]);
    }
  }

  EXPECT_TRUE(positions[10] == expected_positions[0][0]);
  EXPECT_TRUE(positions[11] == expected_positions[0][1]);
  EXPECT_TRUE(positions[12] == expected_positions[1][0]);
  EXPECT_TRUE(positions[13] == expected_positions[2][4]);
}

TEST(curves_editors, DuplicatePointsTwoCyclic)
{
  /* Two points from cyclic curve. Points are on cycle. */
  const Vector<Vector<float3>> expected_positions = {
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}},
      {{0, 0, 0}},
      {{1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {-1, 1, 0}},
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}, {1, -1, 0}}};

  bke::CurvesGeometry curves = create_curves(expected_positions, 4, {2});
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(Array<int>{5, 8}.as_span(), memory);

  duplicate_points(curves, mask);

  EXPECT_TRUE(curves.curves_num() == expected_positions.size() + 1);

  const Span<float3> positions = curves.positions();
  const OffsetIndices points_by_curve = curves.points_by_curve();

  for (const int curve : expected_positions.index_range()) {
    const Span<float3> expected_curve_positions = expected_positions[curve];
    const IndexRange points = points_by_curve[curve];
    for (const int point : expected_curve_positions.index_range()) {
      EXPECT_TRUE(positions[points[point]] == expected_curve_positions[point]);
    }
  }

  EXPECT_TRUE(positions[14] == expected_positions[2][3]);
  EXPECT_TRUE(positions[15] == expected_positions[2][0]);
}

TEST(curves_editors, SplitPointsTwoSingle)
{
  /* Split two points from single curve. */
  const Vector<float3> positions = {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}};

  bke::CurvesGeometry curves = create_curves(positions, 4, {});
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices<int>({1, 2}, memory);

  bke::CurvesGeometry new_curves = split_points(curves, mask);

  const Vector<Vector<float3>> expected_positions = {
      {{-1, 1, 0}, {1, 1, 0}}, {{-1.5, 0, 0}, {-1, 1, 0}}, {{1, 1, 0}, {1.5, 0, 0}}};

  GTEST_ASSERT_EQ(new_curves.curves_num(), expected_positions.size());
  validate_positions(expected_positions, new_curves.points_by_curve(), new_curves.positions());
}

TEST(curves_editors, SplitPointsFourThree)
{
  /* Four points from three curves. One curve has one point. */
  const Vector<Vector<float3>> positions = {
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}},
      {{0, 0, 0}},
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}, {1, -1, 0}}};

  bke::CurvesGeometry curves = create_curves(positions, 4, {});
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(Array<int>{0, 1, 4, 9}.as_span(), memory);

  bke::CurvesGeometry new_curves = split_points(curves, mask);

  const Vector<Vector<float3>> expected_positions = {
      {{-1.5, 0, 0}, {-1, 1, 0}},
      {{-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}},
      {{0, 0, 0}},
      {{1, -1, 0}},
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}, {1, -1, 0}}};

  GTEST_ASSERT_EQ(new_curves.curves_num(), expected_positions.size());
  validate_positions(expected_positions, new_curves.points_by_curve(), new_curves.positions());
}

TEST(curves_editors, SplitPointsTwoCyclic)
{
  /* Two points from cyclic curve. Points are on cycle. */
  const Vector<Vector<float3>> positions = {
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}},
      {{0, 0, 0}},
      {{1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {-1, 1, 0}},
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}, {1, -1, 0}}};

  bke::CurvesGeometry curves = create_curves(positions, 4, {2});
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(Array<int>{5, 8}.as_span(), memory);

  bke::CurvesGeometry new_curves = split_points(curves, mask);

  const Vector<Vector<float3>> expected_positions = {
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}},
      {{0, 0, 0}},
      {{-1, 1, 0}, {1, 1, 0}},
      {{1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {-1, 1, 0}},
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}, {1, -1, 0}}};

  GTEST_ASSERT_EQ(new_curves.curves_num(), expected_positions.size());
  validate_positions(expected_positions, new_curves.points_by_curve(), new_curves.positions());
  Array<bool> expected_cyclic = {false, false, false, false, false};
  VArray<bool> cyclic = new_curves.cyclic();
  for (const int i : expected_cyclic.index_range()) {
    EXPECT_EQ(expected_cyclic[i], cyclic[i]);
  }
}

TEST(curves_editors, SplitPointsTwoTouchCyclic)
{
  /* Two points from cyclic curve. Points are touching cycle. */
  const Vector<Vector<float3>> positions = {
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}},
      {{0, 0, 0}},
      {{1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {-1, 1, 0}},
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}, {1, -1, 0}}};

  bke::CurvesGeometry curves = create_curves(positions, 4, {2});
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(Array<int>{5, 6}.as_span(), memory);

  bke::CurvesGeometry new_curves = split_points(curves, mask);

  const Vector<Vector<float3>> expected_positions = {
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}},
      {{0, 0, 0}},
      {{1, 1, 0}, {1, -1, 0}},
      {{1, -1, 0}, {-1, -1, 0}, {-1, 1, 0}, {1, 1, 0}},
      {{-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}, {1, -1, 0}}};

  GTEST_ASSERT_EQ(new_curves.curves_num(), expected_positions.size());
  validate_positions(expected_positions, new_curves.points_by_curve(), new_curves.positions());
}

TEST(curves_editors, SplitEverySecondCyclic)
{
  /* Split every second point in cyclic curve. Expected result all selected points
   * as separate curves and original curve. */
  const Vector<Vector<float3>> positions = {{{0, -1, 0},
                                             {-1, -1, 0},
                                             {-1, 0, 0},
                                             {-1, 1, 0},
                                             {0, 1, 0},
                                             {1, 1, 0},
                                             {1, 0, 0},
                                             {1, -1, 0}}};

  bke::CurvesGeometry curves = create_curves(positions, 4, {0});
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(Array<int>{0, 2, 4, 6}.as_span(), memory);

  bke::CurvesGeometry new_curves = split_points(curves, mask);

  const Vector<Vector<float3>> expected_positions = {{{0, -1, 0}},
                                                     {{-1, 0, 0}},
                                                     {{0, 1, 0}},
                                                     {{1, 0, 0}},
                                                     {{0, -1, 0},
                                                      {-1, -1, 0},
                                                      {-1, 0, 0},
                                                      {-1, 1, 0},
                                                      {0, 1, 0},
                                                      {1, 1, 0},
                                                      {1, 0, 0},
                                                      {1, -1, 0}}};

  GTEST_ASSERT_EQ(new_curves.curves_num(), expected_positions.size());
  validate_positions(expected_positions, new_curves.points_by_curve(), new_curves.positions());
}

TEST(curves_editors, SplitAllSelectedButFirstCyclic)
{
  /* Split all except first points in cyclic curve. Expected result two curves. One from selected
   * points another from first, second and last. Both not cyclic. */
  const Vector<Vector<float3>> positions = {{{0, -1, 0},
                                             {-1, -1, 0},
                                             {-1, 0, 0},
                                             {-1, 1, 0},
                                             {0, 1, 0},
                                             {1, 1, 0},
                                             {1, 0, 0},
                                             {1, -1, 0}}};

  bke::CurvesGeometry curves = create_curves(positions, 4, {0});
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(Array<int>{1, 2, 3, 4, 5, 6, 7}.as_span(),
                                                 memory);

  bke::CurvesGeometry new_curves = split_points(curves, mask);

  const Vector<Vector<float3>> expected_positions = {
      {{-1, -1, 0}, {-1, 0, 0}, {-1, 1, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {1, -1, 0}},
      {{1, -1, 0}, {0, -1, 0}, {-1, -1, 0}},
  };

  GTEST_ASSERT_EQ(new_curves.curves_num(), expected_positions.size());
  validate_positions(expected_positions, new_curves.points_by_curve(), new_curves.positions());
  EXPECT_EQ(new_curves.curves_num(), expected_positions.size());
  EXPECT_FALSE(new_curves.cyclic()[0]);
  EXPECT_FALSE(new_curves.cyclic()[1]);
}

TEST(curves_editors, SplitTwoOnSeamAndExtraCyclic)
{
  /* Split first, last and pair in the middle. Expected result four non cyclic curves. */
  const Vector<Vector<float3>> positions = {{{0, -1, 0},
                                             {-1, -1, 0},
                                             {-1, 0, 0},
                                             {-1, 1, 0},
                                             {0, 1, 0},
                                             {1, 1, 0},
                                             {1, 0, 0},
                                             {1, -1, 0}}};

  bke::CurvesGeometry curves = create_curves(positions, 4, {0});
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_indices(Array<int>{0, 3, 4, 7}.as_span(), memory);

  bke::CurvesGeometry new_curves = split_points(curves, mask);

  const Vector<Vector<float3>> expected_positions = {
      {{-1, 1, 0}, {0, 1, 0}},
      {{1, -1, 0}, {0, -1, 0}},
      {{0, -1, 0}, {-1, -1, 0}, {-1, 0, 0}, {-1, 1, 0}},
      {{0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {1, -1, 0}}};

  GTEST_ASSERT_EQ(new_curves.curves_num(), expected_positions.size());
  validate_positions(expected_positions, new_curves.points_by_curve(), new_curves.positions());
  EXPECT_FALSE(new_curves.cyclic()[0]);
  EXPECT_FALSE(new_curves.cyclic()[1]);
  EXPECT_FALSE(new_curves.cyclic()[2]);
  EXPECT_FALSE(new_curves.cyclic()[3]);
}

static void test_insert_remove_knots_matrix_identity(const bke::CurvesGeometry &curves,
                                                     const int curve,
                                                     const float knot,
                                                     const int repeat)
{
  const IndexRange curve_points = curves.points_by_curve()[curve];
  const Span<float3> positions = curves.positions().slice(curve_points);
  const int8_t order = curves.nurbs_orders()[curve];
  const bool cyclic = curves.cyclic()[curve];
  const KnotsMode knots_mode = KnotsMode(curves.nurbs_knots_modes()[curve]);
  const int knots_num = bke::curves::nurbs::knots_num(curve_points.size(), order, cyclic);

  Array<float> knots(knots_num);
  bke::curves::nurbs::load_curve_knots(knots_mode,
                                       curve_points.size(),
                                       order,
                                       cyclic,
                                       curves.nurbs_custom_knots_by_curve()[curve],
                                       curves.nurbs_custom_knots(),
                                       knots);

  int knot_span;
  int mult;
  nurbs::find_span_mult(knot, knots, order, knot_span, mult);
  BLI_assert(mult + repeat < order);

  nurbs::WeightMatrix ins_m = nurbs::calc_knot_insertion_weights(
      knots, curve_points.size(), order, knot, knot_span, mult, repeat);

  Array<float> new_knots(knots.size() + repeat);

  nurbs::insert_knot_value(curve_points.size(), knots, knot, knot_span, repeat, new_knots);

  const int last_knot_index = new_knots.rend() -
                              std::find(new_knots.rbegin(), new_knots.rend(), knot) - 1;
  nurbs::WeightMatrix rem_m = nurbs::calc_knot_removal_weights(
      new_knots, ins_m.rows(), order, knot, last_knot_index, mult + repeat, repeat);

  nurbs::WeightMatrix expected_identity = rem_m * ins_m;
  expected_identity.makeCompressed();

  Array<float> weights(curve_points.size());
  weights.fill(1.0f);

  Array<float3> dest_positions(positions.size());
  nurbs::gather_modified_positions(
      positions, weights, expected_identity, positions.index_range(), dest_positions);

  for (const int i : positions.index_range()) {
    EXPECT_TRUE(math::distance(positions[i], dest_positions[i]) < 0.000001);
  }
}

/**
 * Calculates various knot insertion and removal matrices for various curves.
 * Multiplication of corresponding removal and insertion matrices must be close to identity matrix.
 */
TEST(curves_editors, InsertRemoveKnots)
{
  const Vector<float3> positions = {
      {-1.5, 0, 0}, {-1, 1, 0}, {1, 1, 0}, {1.5, 0, 0}, {2.5, 0, 0}, {3.5, 1, 0}};
  for (const int order : IndexRange::from_begin_end_inclusive(2, 6)) {

    const std::array<bke::CurvesGeometry, 2> geom{create_curves(positions, order, {}),
                                                  create_curves(positions, order, {0})};

    for (auto &curves : geom) {
      const IndexRange curve_points = curves.points_by_curve()[0];
      const bool cyclic = curves.cyclic()[0];
      const float first_knot = order - 1;
      const float last_knot = curve_points.size() + (cyclic ? order - 2 : 0);
      for (const int repeat : IndexRange::from_begin_end_inclusive(1, order - 2)) {
        test_insert_remove_knots_matrix_identity(curves, 0, first_knot, repeat);
        test_insert_remove_knots_matrix_identity(curves, 0, last_knot, repeat);
      }

      const float some_knot = first_knot + 0.3f;
      const float some_other = last_knot - 0.4f;
      for (const int repeat : IndexRange::from_begin_end_inclusive(1, order - 1)) {
        test_insert_remove_knots_matrix_identity(curves, 0, some_knot, repeat);
        test_insert_remove_knots_matrix_identity(curves, 0, some_other, repeat);
      }
    }
  }
}

}  // namespace blender::ed::curves::tests
