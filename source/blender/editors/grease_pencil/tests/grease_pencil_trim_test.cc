/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_lasso_2d.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "ED_grease_pencil.hh"
#include "ED_view3d.hh"

namespace blender::ed::greasepencil::tests {

static constexpr int BBOX_PADDING = 0;

static bke::CurvesGeometry create_test_curves(const Span<int> offsets,
                                              const Span<float2> positions_2d,
                                              const Span<bool> cyclic)
{
  BLI_assert(!offsets.is_empty());
  const int curves_num = offsets.size() - 1;
  BLI_assert(cyclic.size() == curves_num);
  const int points_num = offsets.last();

  bke::CurvesGeometry curves(points_num, curves_num);
  curves.offsets_for_write().copy_from(offsets);
  curves.cyclic_for_write().copy_from(cyclic);

  for (const int i : curves.points_range()) {
    curves.positions_for_write()[i] = float3(positions_2d[i], 0.0f);
  }

  return curves;
}

static Array<rcti> calculate_curve_bounds(const OffsetIndices<int> src_offsets,
                                          const Span<float2> positions_2d)
{
  Array<rcti> screen_space_curve_bounds(src_offsets.size());

  for (const int i : src_offsets.index_range()) {
    const IndexRange points = src_offsets[i];
    auto bounds = bounds::min_max(positions_2d.slice(points));
    rcti screen_space_bounds;
    BLI_rcti_init(&screen_space_bounds,
                  int(bounds->min.x),
                  int(bounds->max.x),
                  int(bounds->min.y),
                  int(bounds->max.y));
    BLI_rcti_pad(&screen_space_bounds, BBOX_PADDING, BBOX_PADDING);

    screen_space_curve_bounds[i] = screen_space_bounds;
  }

  return screen_space_curve_bounds;
}

static void expect_near_positions(const blender::Span<float3> actual,
                                  const blender::Span<float2> expected)
{
  EXPECT_EQ(expected.size(), actual.size());
  if (expected.size() != actual.size()) {
    return;
  }

  for (const int64_t i : expected.index_range()) {
    EXPECT_NEAR(expected[i].x, actual[i].x, 1e-4) << "X mismatch at index " << i;
    EXPECT_NEAR(expected[i].y, actual[i].y, 1e-4) << "Y mismatch at index " << i;
  }
}

static bke::CurvesGeometry trim_curve(const bke::CurvesGeometry &src,
                                      const Span<float2> screen_space_positions,
                                      const Span<int2> mcoords,
                                      const bool keep_caps)
{
  const Array<rcti> screen_space_bbox = calculate_curve_bounds(src.points_by_curve(),
                                                               screen_space_positions);
  return trim::trim_curve_segments(src,
                                   screen_space_positions,
                                   screen_space_bbox,
                                   mcoords,
                                   src.curves_range(),
                                   src.curves_range(),
                                   keep_caps);
}

TEST(grease_pencil_trim, trim_two_edges)
{
  using namespace bke::greasepencil;
  using namespace bke;

  const Array<int2> mcoords = {{-1, 5}, {1, 5}, {1, 1}, {5, 1}, {5, -1}, {-1, -1}};
  const Array<int> src_offsets = {0, 2, 4, 6, 8};
  const Array<bool> src_cyclic = {false, false, false, false};
  const Array<float2> screen_space_positions = {{2.0f, 0.0f},
                                                {2.0f, 6.0f},
                                                {4.0f, 0.0f},
                                                {4.0f, 6.0f},
                                                {0.0f, 2.0f},
                                                {6.0f, 2.0f},
                                                {0.0f, 4.0f},
                                                {6.0f, 4.0f}};
  const CurvesGeometry src = create_test_curves(src_offsets, screen_space_positions, src_cyclic);
  const CurvesGeometry dst = trim_curve(src, screen_space_positions, mcoords, true);

  const Array<float2> expected_positions = {{2.0f, 2.0f},
                                            {2.0f, 6.0f},
                                            {4.0f, 2.0f},
                                            {4.0f, 6.0f},
                                            {2.0f, 2.0f},
                                            {6.0f, 2.0f},
                                            {2.0f, 4.0f},
                                            {6.0f, 4.0f}};
  expect_near_positions(dst.positions(), expected_positions);
}

TEST(grease_pencil_trim, trim_plus_intersection)
{
  using namespace bke::greasepencil;
  using namespace bke;

  const Array<int2> mcoords = {{2, -1}, {2, 1}, {4, 1}, {4, -1}};
  const Array<int> src_offsets = {0, 4, 8};
  const Array<bool> src_cyclic = {false, false};
  const Array<float2> screen_space_positions = {{3.0f, 0.0f},
                                                {3.0f, 2.0f},
                                                {3.0f, 4.0f},
                                                {3.0f, 6.0f},
                                                {0.0f, 3.0f},
                                                {2.0f, 3.0f},
                                                {4.0f, 3.0f},
                                                {6.0f, 3.0f}};
  const CurvesGeometry src = create_test_curves(src_offsets, screen_space_positions, src_cyclic);
  const CurvesGeometry dst = trim_curve(src, screen_space_positions, mcoords, true);

  const Array<float2> expected_positions = {{3.0f, 3.0f},
                                            {3.0f, 4.0f},
                                            {3.0f, 6.0f},
                                            {0.0f, 3.0f},
                                            {2.0f, 3.0f},
                                            {4.0f, 3.0f},
                                            {6.0f, 3.0f}};
  expect_near_positions(dst.positions(), expected_positions);
}

TEST(grease_pencil_trim, trim_t_intersection)
{
  using namespace bke::greasepencil;
  using namespace bke;

  const Array<int2> mcoords = {{-1, 2}, {1, 2}, {1, 4}, {-1, 4}};
  const Array<int> src_offsets = {0, 3, 7};
  const Array<bool> src_cyclic = {false, false};
  const Array<float2> screen_space_positions = {{3.0f, 0.0f},
                                                {3.0f, 2.0f},
                                                {3.0f, 3.0f},
                                                {0.0f, 3.0f},
                                                {2.0f, 3.0f},
                                                {4.0f, 3.0f},
                                                {6.0f, 3.0f}};
  const CurvesGeometry src = create_test_curves(src_offsets, screen_space_positions, src_cyclic);
  const CurvesGeometry dst = trim_curve(src, screen_space_positions, mcoords, true);

  const Array<float2> expected_positions = {
      {3.0f, 0.0f}, {3.0f, 2.0f}, {3.0f, 3.0f}, {3.0f, 3.0f}, {4.0f, 3.0f}, {6.0f, 3.0f}};
  expect_near_positions(dst.positions(), expected_positions);
}

TEST(grease_pencil_trim, trim_figure_eight)
{
  using namespace bke::greasepencil;
  using namespace bke;

  const Array<int2> mcoords = {{4, 2}, {4, 4}, {6, 4}, {6, 2}};
  const Array<int> src_offsets = {0, 8};
  const Array<bool> src_cyclic = {true};
  const Array<float2> screen_space_positions = {{0.0f, 1.0f},
                                                {0.0f, 3.0f},
                                                {2.0f, 3.0f},
                                                {3.0f, 1.0f},
                                                {5.0f, 1.0f},
                                                {5.0f, 3.0f},
                                                {3.0f, 3.0f},
                                                {2.0f, 1.0f}};
  const CurvesGeometry src = create_test_curves(src_offsets, screen_space_positions, src_cyclic);
  const CurvesGeometry dst = trim_curve(src, screen_space_positions, mcoords, true);

  const Array<float2> expected_positions = {
      {2.5f, 2.0f}, {2.0f, 3.0f}, {0.0f, 3.0f}, {0.0f, 1.0f}, {2.0f, 1.0f}, {2.5f, 2.0f}};
  expect_near_positions(dst.positions(), expected_positions);
}

}  // namespace blender::ed::greasepencil::tests
