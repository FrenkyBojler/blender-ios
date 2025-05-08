/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "testing/testing.h"

#include "BKE_brush.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "DNA_brush_types.h"

class BrushTest : public testing::Test {
 public:
  Main *bmain = nullptr;

  static void SetUpTestSuite()
  {
    BKE_idtype_init();
  }

  void SetUp() override
  {
    bmain = BKE_main_new();
  }

  void TearDown() override
  {
    BKE_main_free(bmain);

  }
};

static void check_id_and_name(ID* a, ID* b)
{
  ASSERT_NE(a, b);
  ASSERT_FALSE(STREQ(a->name, b->name));
}

TEST_F(BrushTest, deep_copy)
{
  Brush *brush = BKE_brush_add(bmain, "UnitTestBrush", OB_MODE_SCULPT);
  brush->paint_curve = static_cast<PaintCurve *>(BKE_id_new(bmain, ID_PC, "UnitTestPaintCurve"));
  brush->mtex.tex = static_cast<Tex *>(BKE_id_new(bmain, ID_TE, "UnitTestTexture"));
  brush->mtex.tex->ima = static_cast<Image *>(BKE_id_new(bmain, ID_IM, "UnitTestImage"));

  Brush *duplicated_brush = BKE_brush_duplicate(bmain, brush);

  check_id_and_name(&brush->id, &duplicated_brush->id);
  check_id_and_name(&brush->paint_curve->id, &duplicated_brush->paint_curve->id);
  check_id_and_name(&brush->mtex.tex->id, &duplicated_brush->mtex.tex->id);
  check_id_and_name(&brush->mtex.tex->ima->id, &duplicated_brush->mtex.tex->ima->id);
}
