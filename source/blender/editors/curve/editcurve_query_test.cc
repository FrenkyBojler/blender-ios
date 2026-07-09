/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_listbase.hh"
#include "BLI_math_vector_c.hh"

#include "BKE_curve.hh"
#include "BKE_gtest_base.hh"
#include "BKE_main.hh"

#include "DNA_curve_enums.h"
#include "DNA_curve_types.h"

#include "ED_curve.hh"

#include "MEM_guardedalloc.h"

#include "testing/testing.h"

namespace blender::ed::curve::tests {

class CurveActiveCenterTest : public bke::BlenderGTestBase {
 public:
  Main *bmain = nullptr;
  Curve *curve = nullptr;
  Nurb *nurb = nullptr;
  BezTriple *bezt = nullptr;

  void SetUp() override
  {
    bmain = BKE_main_new();
    curve = BKE_curve_add(bmain, "Curve", OB_CURVES_LEGACY);

    curve->editnurb = MEM_new<EditNurb>("editnurb test");
    nurb = MEM_new<Nurb>("nurb test");
    nurb->type = CU_BEZIER;
    nurb->pntsu = 0;
    nurb->pntsv = 1;
    BKE_nurb_bezierPoints_add(nurb, 1);
    bezt = &nurb->bezt[0];

    /* Distinct positions so wrong centers are easy to spot. */
    copy_v3_fl3(bezt->vec[0], -1.5f, -0.5f, 0.0f); /* Left handle. */
    copy_v3_fl3(bezt->vec[1], -1.0f, 0.0f, 0.0f);  /* Control point. */
    copy_v3_fl3(bezt->vec[2], -0.5f, 0.5f, 0.0f);  /* Right handle. */

    BLI_addtail(&curve->editnurb->nurbs, nurb);
    BKE_curve_nurb_vert_active_set(curve, nurb, bezt);
  }

  void TearDown() override
  {
    BKE_main_free(bmain);
  }

  void clear_bezt_selection()
  {
    BEZT_DESEL_ALL(bezt);
  }
};

/* Left-handle-only selection must use the left handle as the active center. */
TEST_F(CurveActiveCenterTest, LeftHandleOnlyUsesLeft)
{
  clear_bezt_selection();
  bezt->f1 |= BEZT_FLAG_SELECT;

  float center[3];
  ASSERT_TRUE(ED_curve_active_center(curve, center));
  EXPECT_V3_NEAR(center, bezt->vec[0], 1e-6f);
}

/* Right-handle-only selection must use the right handle. */
TEST_F(CurveActiveCenterTest, RightHandleOnlyUsesRight)
{
  clear_bezt_selection();
  bezt->f3 |= BEZT_FLAG_SELECT;

  float center[3];
  ASSERT_TRUE(ED_curve_active_center(curve, center));
  EXPECT_V3_NEAR(center, bezt->vec[2], 1e-6f);
}

/* Control-point selection keeps the knot as the active center. */
TEST_F(CurveActiveCenterTest, KnotOnlyUsesKnot)
{
  clear_bezt_selection();
  bezt->f2 |= BEZT_FLAG_SELECT;

  float center[3];
  ASSERT_TRUE(ED_curve_active_center(curve, center));
  EXPECT_V3_NEAR(center, bezt->vec[1], 1e-6f);
}

/* Both handles selected (no exclusive single handle) fall back to the knot. */
TEST_F(CurveActiveCenterTest, BothHandlesUsesKnot)
{
  clear_bezt_selection();
  bezt->f1 |= BEZT_FLAG_SELECT;
  bezt->f3 |= BEZT_FLAG_SELECT;

  float center[3];
  ASSERT_TRUE(ED_curve_active_center(curve, center));
  EXPECT_V3_NEAR(center, bezt->vec[1], 1e-6f);
}

/* Mixed handle + knot selection also keeps the knot. */
TEST_F(CurveActiveCenterTest, LeftAndKnotUsesKnot)
{
  clear_bezt_selection();
  bezt->f1 |= BEZT_FLAG_SELECT;
  bezt->f2 |= BEZT_FLAG_SELECT;

  float center[3];
  ASSERT_TRUE(ED_curve_active_center(curve, center));
  EXPECT_V3_NEAR(center, bezt->vec[1], 1e-6f);
}

/* Without an active vertex there is no center to return. */
TEST_F(CurveActiveCenterTest, NoActiveVertFails)
{
  curve->actvert = CU_ACT_NONE;

  float center[3];
  EXPECT_FALSE(ED_curve_active_center(curve, center));
}

}  // namespace blender::ed::curve::tests
