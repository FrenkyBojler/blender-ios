/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"

#include "BKE_curves.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"

#include "GEO_interpolate_curves.hh"

#include "testing/testing.h"

namespace blender::bke::tests {

class GreasePencilInterpolate : public testing::Test {
 public:
  enum class TestCurveShape {
    Zero,
    Circle,
    Eight,
    Helix,
  };

  static void create_test_shape(const TestCurveShape shape, MutableSpan<float3> positions)
  {
    switch (shape) {
      case TestCurveShape::Zero:
        positions.fill(float3(0.0f));
        break;
      case TestCurveShape::Circle:
        for (const int point_i : positions.index_range()) {
          const float angle = 2.0f * M_PI * float(point_i) / float(positions.size());
          positions[point_i] = float3(math::cos(angle), math::sin(angle), 0.0f);
        }
        break;
      case TestCurveShape::Eight:
        for (const int point_i : positions.index_range()) {
          const float angle = 2.0f * M_PI * float(point_i) / float(positions.size());
          positions[point_i] = float3(math::cos(angle), math::sin(angle * 2.0f), 0.0f);
        }
        break;
      case TestCurveShape::Helix:
        const int turns = 3;
        const float pitch = 0.3f;
        for (const int point_i : positions.index_range()) {
          const float factor = float(turns) * float(point_i) / float(positions.size() - 1);
          const float angle = 2.0f * M_PI * factor;
          const float height = pitch * factor;
          positions[point_i] = float3(math::cos(angle), math::sin(angle), height);
        }
        break;
    }
  }

  static bke::CurvesGeometry create_test_curves(Span<int> offsets,
                                                Span<bool> cyclic,
                                                TestCurveShape shape)
  {
    BLI_assert(!offsets.is_empty());
    const int curves_num = offsets.size() - 1;
    BLI_assert(cyclic.size() == curves_num);
    const int points_num = offsets.last();

    bke::CurvesGeometry curves(points_num, curves_num);
    curves.offsets_for_write().copy_from(offsets);
    curves.cyclic_for_write().copy_from(cyclic);

    MutableSpan<float3> positions = curves.positions_for_write();
    for (const int curve_i : curves.curves_range()) {
      const IndexRange points = curves.points_by_curve()[curve_i];
      create_test_shape(shape, positions.slice(points));
    }

    /* Attribute storing original indices to test point remapping. */
    SpanAttributeWriter<int> test_indices_writer =
        curves.attributes_for_write().lookup_or_add_for_write_span<int>(
            "test_index", bke::AttrDomain::Point, bke::AttributeInitConstruct());
    array_utils::fill_index_range(test_indices_writer.span);
    test_indices_writer.finish();

    return curves;
  }

  void test_sample_curve(const bke::CurvesGeometry &curves,
                         const int curve_index,
                         const bool reverse,
                         const Span<int> expected_indices,
                         const Span<float> expected_factors)
  {
    const int num_dst_points = expected_indices.size();
    BLI_assert(expected_factors.size() == num_dst_points);

    const bool cyclic = curves.cyclic()[curve_index];

    Array<int> indices(num_dst_points, -9999);
    Array<float> factors(num_dst_points, -12345.6f);
    geometry::sample_curve_padded(curves, curve_index, cyclic, reverse, indices, factors);

    EXPECT_EQ_SPAN(expected_indices, indices.as_span());
    EXPECT_EQ_SPAN(expected_factors, factors.as_span());
  }
};

TEST_F(GreasePencilInterpolate, sample_curve_empty_output)
{
  bke::CurvesGeometry curves = create_test_curves(
      {0, 1, 3}, {false, false}, TestCurveShape::Eight);

  test_sample_curve(curves, 0, false, {}, {});
  test_sample_curve(curves, 1, false, {}, {});
}

TEST_F(GreasePencilInterpolate, sample_curve_same_length)
{
  bke::CurvesGeometry curves = create_test_curves(
      {0, 1, 3, 13, 14, 16, 26}, {false, false, false, true, true, true}, TestCurveShape::Eight);

  test_sample_curve(curves, 0, false, {0}, {0.0f});
  test_sample_curve(curves, 0, true, {0}, {1.0f});

  test_sample_curve(curves, 1, false, {0, 1}, {0.0f, 0.0f});
  test_sample_curve(curves, 1, true, {0, 1}, {1.0f, 1.0f});

  test_sample_curve(curves,
                    2,
                    false,
                    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
                    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
  test_sample_curve(curves,
                    2,
                    true,
                    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
                    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});

  test_sample_curve(curves, 3, false, {0}, {0.0f});
  test_sample_curve(curves, 3, true, {0}, {1.0f});

  test_sample_curve(curves, 4, false, {0, 1}, {0.0f, 0.0f});
  test_sample_curve(curves, 4, true, {0, 1}, {1.0f, 1.0f});

  test_sample_curve(curves,
                    5,
                    false,
                    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
                    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
  test_sample_curve(curves,
                    5,
                    true,
                    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
                    {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
}

TEST_F(GreasePencilInterpolate, sample_curve_shorter)
{
  bke::CurvesGeometry curves = create_test_curves(
      {0, 1, 3, 13, 14, 16, 26}, {false, false, false, true, true, true}, TestCurveShape::Eight);

  //   test_sample_curve(curves, 0, false, {}, {});
  //   test_sample_curve(curves, 0, true, {}, {});

  test_sample_curve(curves, 1, false, {0}, {0.0f});
  test_sample_curve(curves, 1, true, {0}, {1.0f});

  test_sample_curve(curves, 2, false, {0, 3, 6, 9}, {0.0f, 0.0f, 0.0f, 0.0f});
  test_sample_curve(curves, 2, true, {0, 3, 6, 9}, {1.0f, 1.0f, 1.0f, 1.0f});

  //   test_sample_curve(curves, 3, false, {}, {});
  //   test_sample_curve(curves, 3, true, {}, {});

  test_sample_curve(curves, 4, false, {0}, {0.0f});
  test_sample_curve(curves, 4, true, {0}, {1.0f});

  test_sample_curve(curves, 5, false, {0, 2, 5, 7}, {0.0f, 0.5f, 0.0f, 0.5f});
  test_sample_curve(curves, 5, true, {0, 5, 7, 9}, {1.0f, 0.5f, 1.0f, 0.5f});
}

TEST_F(GreasePencilInterpolate, sample_zero_length_curve)
{
  bke::CurvesGeometry curves = create_test_curves(
      {0, 10, 20}, {false, true}, TestCurveShape::Zero);

  test_sample_curve(curves,
                    0,
                    false,
                    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
                    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
  test_sample_curve(curves,
                    1,
                    false,
                    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
                    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
}

}  // namespace blender::bke::tests
