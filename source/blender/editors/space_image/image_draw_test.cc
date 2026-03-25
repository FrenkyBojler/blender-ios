/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spimage
 */

#include "image_intern.hh"

#include "DNA_scene_types.h"

#include "testing/testing.h"

namespace blender::ed::space_image::tests {

/* -------------------------------------------------------------------- */
/** \name render_border_get
 * \{ */

TEST(ImageDraw, RenderBorderGet_NoBorderFlag)
{
  /* R_BORDER not set: should return nullopt regardless of the border rect. */
  Scene scene = {};
  scene.r.border = {0.25f, 0.75f, 0.25f, 0.75f};
  scene.r.mode = 0;

  EXPECT_FALSE(render_border_get(&scene).has_value());
}

TEST(ImageDraw, RenderBorderGet_BorderFlagSet_FullFrame)
{
  /* R_BORDER set with the default full-frame border (0..1). */
  Scene scene = {};
  scene.r.border = {0.0f, 1.0f, 0.0f, 1.0f};
  scene.r.mode = R_BORDER;

  const std::optional<rctf> result = render_border_get(&scene);
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(result->xmin, 0.0f);
  EXPECT_FLOAT_EQ(result->xmax, 1.0f);
  EXPECT_FLOAT_EQ(result->ymin, 0.0f);
  EXPECT_FLOAT_EQ(result->ymax, 1.0f);
}

TEST(ImageDraw, RenderBorderGet_BorderFlagSet_PartialRegion)
{
  /* R_BORDER set with a sub-region border. */
  Scene scene = {};
  scene.r.border = {0.25f, 0.75f, 0.1f, 0.9f};
  scene.r.mode = R_BORDER;

  const std::optional<rctf> result = render_border_get(&scene);
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(result->xmin, 0.25f);
  EXPECT_FLOAT_EQ(result->xmax, 0.75f);
  EXPECT_FLOAT_EQ(result->ymin, 0.1f);
  EXPECT_FLOAT_EQ(result->ymax, 0.9f);
}

TEST(ImageDraw, RenderBorderGet_OtherModeFlagsDoNotEnable)
{
  /* Other render mode flags must not trigger a render border. */
  Scene scene = {};
  scene.r.border = {0.25f, 0.75f, 0.25f, 0.75f};
  scene.r.mode = R_CROP | R_SIMPLIFY;

  EXPECT_FALSE(render_border_get(&scene).has_value());
}

TEST(ImageDraw, RenderBorderGet_BorderFlagCombinedWithOthers)
{
  /* R_BORDER combined with other flags still returns the border. */
  Scene scene = {};
  scene.r.border = {0.1f, 0.9f, 0.2f, 0.8f};
  scene.r.mode = R_BORDER | R_CROP;

  const std::optional<rctf> result = render_border_get(&scene);
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(result->xmin, 0.1f);
  EXPECT_FLOAT_EQ(result->xmax, 0.9f);
  EXPECT_FLOAT_EQ(result->ymin, 0.2f);
  EXPECT_FLOAT_EQ(result->ymax, 0.8f);
}

/** \} */

}  // namespace blender::ed::space_image::tests
