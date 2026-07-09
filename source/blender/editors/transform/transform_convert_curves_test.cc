/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_index_mask.hh"
#include "BLI_math_matrix_types.hh"

#include "BKE_attribute.hh"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"
#include "BKE_gtest_base.hh"

#include "DNA_curve_types.h"
#include "DNA_view3d_types.h"

#include "ED_curves.hh"

#include "MEM_guardedalloc.h"

#include "testing/testing.h"

#include "transform.hh"
#include "transform_convert.hh"

namespace blender::ed::transform::tests {

class CurvesHandleCenterTest : public bke::BlenderGTestBase {
 public:
  bke::CurvesGeometry curves;
  TransInfo t{};
  TransDataContainer tc{};
  IndexMaskMemory memory;

  /* Distinct positions so a wrong center is easy to spot. */
  const float3 control_point{-1.0f, 0.0f, 0.0f};
  const float3 left_handle{-1.5f, -0.5f, 0.0f};
  const float3 right_handle{-0.5f, 0.5f, 0.0f};

  void SetUp() override
  {
    curves = bke::CurvesGeometry(1, 1);
    curves.fill_curve_types(CURVE_TYPE_BEZIER);
    curves.offsets_for_write().last() = 1;

    curves.positions_for_write()[0] = control_point;
    curves.handle_positions_left_for_write()[0] = left_handle;
    curves.handle_positions_right_for_write()[0] = right_handle;
    curves.handle_types_left_for_write()[0] = BEZIER_HANDLE_ALIGN;
    curves.handle_types_right_for_write()[0] = BEZIER_HANDLE_ALIGN;

    /* Lone left-handle selection: control and right handle unselected. */
    {
      bke::GSpanAttributeWriter selection = ed::curves::ensure_selection_attribute(
          curves, bke::AttrDomain::Point, bke::AttrType::Bool, ".selection");
      selection.span.typed<bool>().fill(false);
      selection.finish();
    }
    set_handle_selection(true, false);

    t.around = V3D_AROUND_CENTER_MEDIAN;
    t.spacetype = 0; /* Not SPACE_VIEW3D: handles stay visible for center logic. */
  }

  void TearDown() override
  {
    free_trans_data();
  }

  void free_trans_data()
  {
    if (tc.custom.type.free_cb != nullptr) {
      tc.custom.type.free_cb(&t, &tc, &tc.custom.type);
      tc.custom.type.free_cb = nullptr;
      tc.custom.type.data = nullptr;
    }
    if (tc.data != nullptr) {
      MEM_delete(tc.data);
      tc.data = nullptr;
    }
    tc.data_len = 0;
  }

  void set_handle_selection(const bool left_selected, const bool right_selected)
  {
    {
      bke::GSpanAttributeWriter selection = ed::curves::ensure_selection_attribute(
          curves, bke::AttrDomain::Point, bke::AttrType::Bool, ".selection_handle_left");
      selection.span.typed<bool>().fill(left_selected);
      selection.finish();
    }
    {
      bke::GSpanAttributeWriter selection = ed::curves::ensure_selection_attribute(
          curves, bke::AttrDomain::Point, bke::AttrType::Bool, ".selection_handle_right");
      selection.span.typed<bool>().fill(right_selected);
      selection.finish();
    }
  }

  /**
   * Populate TransData for a lone handle selection.
   * Matches createTransCurvesVerts: empty control-point mask, one handle selected.
   */
  void populate_handle_only(const eTfmMode mode, const bool left_handle)
  {
    free_trans_data();
    set_handle_selection(left_handle, !left_handle);

    t.mode = mode;

    CurvesTransformData *curves_transform_data = curves::create_curves_transform_custom_data(
        tc.custom.type);

    const IndexMask empty;
    const IndexMask handle_only(IndexRange::from_single(0));
    const std::array<IndexMask, 3> points_to_transform = left_handle ?
                                                             std::array{empty, handle_only, empty} :
                                                             std::array{empty, empty, handle_only};

    tc.data_len = 1;
    tc.data = MEM_new_array_zeroed<TransData>(tc.data_len, __func__);
    curves_transform_data->positions.reinitialize(tc.data_len);

    const bke::crazyspace::GeometryDeformation deformation{curves.positions(), {}};
    const IndexMask bezier_curves(IndexRange::from_single(0));

    curves::curve_populate_trans_data_structs(t,
                                              tc,
                                              curves,
                                              float4x4::identity(),
                                              deformation,
                                              std::nullopt,
                                              points_to_transform,
                                              bezier_curves,
                                              false,
                                              bezier_curves);
  }

  void populate_left_handle_only(const eTfmMode mode)
  {
    populate_handle_only(mode, true);
  }

  void populate_right_handle_only(const eTfmMode mode)
  {
    populate_handle_only(mode, false);
  }
};

/* Translation must keep the handle as TransData.center so snap sources track the handle (#161215). */
TEST_F(CurvesHandleCenterTest, TranslationUsesHandleCenter)
{
  populate_left_handle_only(TFM_TRANSLATION);

  ASSERT_EQ(tc.data_len, 1);
  EXPECT_V3_NEAR(tc.data[0].center, left_handle, 1e-6f);
  EXPECT_V3_NEAR(tc.data[0].iloc, left_handle, 1e-6f);
}

/* Rotation pivots a lone handle around its control point (#144423). */
TEST_F(CurvesHandleCenterTest, RotationUsesControlPointCenter)
{
  populate_left_handle_only(TFM_ROTATION);

  ASSERT_EQ(tc.data_len, 1);
  EXPECT_V3_NEAR(tc.data[0].center, control_point, 1e-6f);
  EXPECT_V3_NEAR(tc.data[0].iloc, left_handle, 1e-6f);
}

/* Scale matches rotation: pivot around the control point. */
TEST_F(CurvesHandleCenterTest, ResizeUsesControlPointCenter)
{
  populate_left_handle_only(TFM_RESIZE);

  ASSERT_EQ(tc.data_len, 1);
  EXPECT_V3_NEAR(tc.data[0].center, control_point, 1e-6f);
  EXPECT_V3_NEAR(tc.data[0].iloc, left_handle, 1e-6f);
}

/* Right-handle-only translation also keeps the handle as center. */
TEST_F(CurvesHandleCenterTest, TranslationRightHandleUsesHandleCenter)
{
  populate_right_handle_only(TFM_TRANSLATION);

  ASSERT_EQ(tc.data_len, 1);
  EXPECT_V3_NEAR(tc.data[0].center, right_handle, 1e-6f);
}

}  // namespace blender::ed::transform::tests
