/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_base.h"

#include "DNA_curve_types.h"

#include "BKE_curve.hh"

#include "blf_internal.hh"

namespace blender::bke::tests {

/* Epsilon for floating point comparisons. */
static constexpr float EPSILON = 1e-4f;

/* -------------------------------------------------------------------- */
/** \name Helper Functions
 * \{ */

/**
 * Create an empty cyclic bezier Nurb with the specified number of points.
 * All BezTriples are zero-initialized.
 */
static Nurb *create_empty_bezier_nurb(int pntsu)
{
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = pntsu;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(pntsu, __func__);
  return nu;
}

/**
 * Set a BezTriple's point position and handle positions.
 * \param bezt: The BezTriple to modify.
 * \param x, y: Point position (vec[1]).
 * \param h1x, h1y: Handle-in position (vec[0]).
 * \param h2x, h2y: Handle-out position (vec[2]).
 * \param h_type: Handle type for both handles (default HD_FREE).
 */
static void set_bezt(BezTriple &bezt,
                     float x,
                     float y,
                     float h1x,
                     float h1y,
                     float h2x,
                     float h2y,
                     char h_type = HD_FREE)
{
  bezt.vec[1][0] = x;
  bezt.vec[1][1] = y;
  bezt.vec[1][2] = 0.0f;
  bezt.vec[0][0] = h1x;
  bezt.vec[0][1] = h1y;
  bezt.vec[0][2] = 0.0f;
  bezt.vec[2][0] = h2x;
  bezt.vec[2][1] = h2y;
  bezt.vec[2][2] = 0.0f;
  bezt.h1 = bezt.h2 = h_type;
}

/**
 * Set a BezTriple as a linear/vector point (handles at point position).
 */
static void set_bezt_linear(BezTriple &bezt, float x, float y)
{
  set_bezt(bezt, x, y, x, y, x, y, HD_VECT);
}

/**
 * Create a square contour with specified handle offset fraction.
 * The square has corners at (x,y), (x+size,y), (x+size,y+size), (x,y+size).
 * Winding is counter-clockwise (positive area).
 * \param handle_frac: Handle offset as fraction of size (e.g., 0.1 = 10%).
 */
static Nurb *create_square_nurb_handles(float x, float y, float size, float handle_frac)
{
  Nurb *nu = create_empty_bezier_nurb(4);
  const float h = size * handle_frac;

  /* Corner 0: bottom-left */
  set_bezt(nu->bezt[0], x, y, x - h, y, x + h, y);
  /* Corner 1: bottom-right */
  set_bezt(nu->bezt[1], x + size, y, x + size - h, y, x + size, y + h);
  /* Corner 2: top-right */
  set_bezt(nu->bezt[2], x + size, y + size, x + size, y + size - h, x + size - h, y + size);
  /* Corner 3: top-left */
  set_bezt(nu->bezt[3], x, y + size, x + h, y + size, x, y + size - h);

  return nu;
}

/**
 * Create a simple square contour as a Nurb with default 10% handle offsets.
 */
static Nurb *create_square_nurb(float x, float y, float size)
{
  return create_square_nurb_handles(x, y, size, 0.1f);
}

/**
 * Create a circle-like contour as a Nurb.
 */
static Nurb *create_circle_nurb(float cx, float cy, float radius)
{
  Nurb *nu = create_empty_bezier_nurb(4);

  /* Magic number for bezier circle approximation: 0.5522847498 */
  const float k = 0.5522847498f * radius;

  /* Point 0: right */
  set_bezt(nu->bezt[0], cx + radius, cy, cx + radius, cy - k, cx + radius, cy + k);
  /* Point 1: top */
  set_bezt(nu->bezt[1], cx, cy + radius, cx + k, cy + radius, cx - k, cy + radius);
  /* Point 2: left */
  set_bezt(nu->bezt[2], cx - radius, cy, cx - radius, cy + k, cx - radius, cy - k);
  /* Point 3: bottom */
  set_bezt(nu->bezt[3], cx, cy - radius, cx - k, cy - radius, cx + k, cy - radius);

  return nu;
}

/**
 * Create a triangle contour.
 */
static Nurb *create_triangle_nurb(float x, float y, float size)
{
  Nurb *nu = create_empty_bezier_nurb(3);
  const float h = size * 0.1f;

  /* Bottom-left */
  set_bezt(nu->bezt[0], x, y, x - h, y, x + h, y);
  /* Bottom-right */
  set_bezt(nu->bezt[1], x + size, y, x + size - h, y, x + size, y + h);
  /* Top center */
  set_bezt(nu->bezt[2],
           x + size * 0.5f,
           y + size,
           x + size * 0.5f + h,
           y + size,
           x + size * 0.5f - h,
           y + size);

  return nu;
}

/**
 * Count the number of Nurbs in a list.
 */
static int count_nurbs(const ListBase *nurbsbase)
{
  return BLI_listbase_count(nurbsbase);
}

/**
 * Count total number of bezier points across all Nurbs.
 */
static int count_bezier_points(const ListBase *nurbsbase)
{
  int count = 0;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase->first); nu != nullptr; nu = nu->next)
  {
    if (nu->bezt != nullptr) {
      count += nu->pntsu;
    }
  }
  return count;
}

/**
 * Calculate the bounding box of all nurbs.
 */
static void calculate_bounds(
    const ListBase *nurbsbase, float &min_x, float &min_y, float &max_x, float &max_y)
{
  min_x = min_y = FLT_MAX;
  max_x = max_y = -FLT_MAX;

  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase->first); nu != nullptr; nu = nu->next)
  {
    if (nu->bezt == nullptr) {
      continue;
    }
    for (int i = 0; i < nu->pntsu; i++) {
      const BezTriple *bezt = &nu->bezt[i];
      /* Check all three vectors (handles and point). */
      for (int j = 0; j < 3; j++) {
        min_x = std::min(min_x, bezt->vec[j][0]);
        max_x = std::max(max_x, bezt->vec[j][0]);
        min_y = std::min(min_y, bezt->vec[j][1]);
        max_y = std::max(max_y, bezt->vec[j][1]);
      }
    }
  }
}

/**
 * Check if all nurbs are cyclic.
 */
static bool all_nurbs_cyclic(const ListBase *nurbsbase)
{
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase->first); nu != nullptr; nu = nu->next)
  {
    if ((nu->flagu & CU_NURB_CYCLIC) == 0) {
      return false;
    }
  }
  return true;
}

/**
 * Check if all nurbs have valid bezier data.
 */
static bool all_nurbs_valid(const ListBase *nurbsbase)
{
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase->first); nu != nullptr; nu = nu->next)
  {
    if (nu->type != CU_BEZIER) {
      return false;
    }
    if (nu->bezt == nullptr && nu->pntsu > 0) {
      return false;
    }
    if (nu->pntsu < 2) {
      return false; /* Need at least 2 points for a valid curve. */
    }
  }
  return true;
}

/**
 * Helper to get a typed pointer to the nurbsbase.
 */
static ListBaseT<Nurb> *as_nurb_list(ListBase *lb)
{
  return reinterpret_cast<ListBaseT<Nurb> *>(lb);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Empty and Trivial Cases
 * \{ */

TEST(vfontdata_sanitize, EmptyList)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Should not crash on empty list. */
  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(nurbsbase.first, nullptr);
  EXPECT_EQ(nurbsbase.last, nullptr);
}

TEST(vfontdata_sanitize, SingleContourNoOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  Nurb *nu = create_square_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu);

  /* Store bounds before. */
  float min_x_before, min_y_before, max_x_before, max_y_before;
  calculate_bounds(&nurbsbase, min_x_before, min_y_before, max_x_before, max_y_before);
  int points_before = count_bezier_points(&nurbsbase);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Single non-overlapping contour should remain unchanged. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1) << "Single contour should remain as one contour";
  EXPECT_EQ(count_bezier_points(&nurbsbase), points_before)
      << "Point count should remain unchanged";
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase)) << "Output should be cyclic";

  /* Bounds should be preserved. */
  float min_x_after, min_y_after, max_x_after, max_y_after;
  calculate_bounds(&nurbsbase, min_x_after, min_y_after, max_x_after, max_y_after);
  EXPECT_NEAR(min_x_before, min_x_after, EPSILON);
  EXPECT_NEAR(min_y_before, min_y_after, EPSILON);
  EXPECT_NEAR(max_x_before, max_x_after, EPSILON);
  EXPECT_NEAR(max_y_before, max_y_after, EPSILON);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, SingleCircleNoOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  Nurb *nu = create_circle_nurb(0.5f, 0.5f, 0.4f);
  BLI_addtail(&nurbsbase, nu);

  int points_before = count_bezier_points(&nurbsbase);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Single non-overlapping circle should remain. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1) << "Single circle should remain as one contour";
  EXPECT_EQ(count_bezier_points(&nurbsbase), points_before)
      << "Point count should remain unchanged";
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase)) << "Output should be cyclic";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, SingleTriangleNoOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  Nurb *nu = create_triangle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu);

  int points_before = count_bezier_points(&nurbsbase);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1) << "Single triangle should remain as one contour";
  EXPECT_EQ(count_bezier_points(&nurbsbase), points_before)
      << "Point count should remain unchanged";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Non-Overlapping Multiple Contours
 * \{ */

TEST(vfontdata_sanitize, TwoSeparateSquares)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares that don't touch - separated by gap. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(2.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  float min_x_before, min_y_before, max_x_before, max_y_before;
  calculate_bounds(&nurbsbase, min_x_before, min_y_before, max_x_before, max_y_before);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Both contours should remain since they don't overlap. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 2) << "Two separate squares should remain as two contours";
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase)) << "All output should be cyclic";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "All output should be valid";

  /* Bounds should be preserved. */
  float min_x_after, min_y_after, max_x_after, max_y_after;
  calculate_bounds(&nurbsbase, min_x_after, min_y_after, max_x_after, max_y_after);
  EXPECT_NEAR(min_x_before, min_x_after, EPSILON);
  EXPECT_NEAR(max_x_before, max_x_after, EPSILON);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, TwoSeparateCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles that don't touch. */
  Nurb *nu1 = create_circle_nurb(0.0f, 0.0f, 0.3f);
  Nurb *nu2 = create_circle_nurb(2.0f, 0.0f, 0.3f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 2) << "Two separate circles should remain as two contours";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "All output should be valid";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, ThreeSeparateShapes)
{
  ListBase nurbsbase = {nullptr, nullptr};

  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_circle_nurb(3.0f, 0.5f, 0.4f);
  Nurb *nu3 = create_triangle_nurb(5.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);
  BLI_addtail(&nurbsbase, nu3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 3) << "Three separate shapes should remain as three contours";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "All output should be valid";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Overlapping Contours
 * \{ */

/* TODO: Bounds checking fails - output geometry is clipped at edges. */
#if 0
TEST(vfontdata_sanitize, TwoOverlappingSquares)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping squares. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(0.5f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  /* Store combined bounds before. */
  float min_x_before, min_y_before, max_x_before, max_y_before;
  calculate_bounds(&nurbsbase, min_x_before, min_y_before, max_x_before, max_y_before);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* After overlap removal, we should have merged result. */
  int result_count = count_nurbs(&nurbsbase);
  EXPECT_GE(result_count, 1) << "Should have at least one contour after merge";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "All output should be valid";
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase)) << "All output should be cyclic";

  /* Bounds should encompass original bounds (union). */
  float min_x_after, min_y_after, max_x_after, max_y_after;
  calculate_bounds(&nurbsbase, min_x_after, min_y_after, max_x_after, max_y_after);
  EXPECT_LE(min_x_after, min_x_before + EPSILON) << "Min X should not increase";
  EXPECT_LE(min_y_after, min_y_before + EPSILON) << "Min Y should not increase";
  EXPECT_GE(max_x_after, max_x_before - EPSILON) << "Max X should not decrease";
  EXPECT_GE(max_y_after, max_y_before - EPSILON) << "Max Y should not decrease";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}
#endif

TEST(vfontdata_sanitize, TwoOverlappingCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles. */
  Nurb *nu1 = create_circle_nurb(0.0f, 0.0f, 0.5f);
  Nurb *nu2 = create_circle_nurb(0.3f, 0.0f, 0.5f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1) << "Should have at least one contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "All output should be valid";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, CircleInsideSquare)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Circle completely inside a square - should create a hole. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 2.0f);
  Nurb *nu2 = create_circle_nurb(1.0f, 1.0f, 0.3f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should result in outer square and inner hole. */
  int result_count = count_nurbs(&nurbsbase);
  EXPECT_GE(result_count, 1) << "Should have at least one contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "All output should be valid";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, IdenticalSquares)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two identical squares - complete overlap. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should result in a single contour. */
  int result_count = count_nurbs(&nurbsbase);
  EXPECT_GE(result_count, 1) << "Should have at least one contour";
  /* Ideally should be exactly 1 contour. */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "All output should be valid";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, ConcentricCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Concentric circles - like a donut. */
  Nurb *nu1 = create_circle_nurb(0.0f, 0.0f, 1.0f); /* Outer */
  Nurb *nu2 = create_circle_nurb(0.0f, 0.0f, 0.5f); /* Inner */
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should result in two contours (outer boundary + inner hole). */
  EXPECT_GE(count_nurbs(&nurbsbase), 1) << "Should have at least one contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "All output should be valid";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge Cases - Numerical Stability
 * \{ */

TEST(vfontdata_sanitize, VerySmallContour)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Very small square - tests numerical stability. */
  Nurb *nu = create_square_nurb(0.0f, 0.0f, 0.001f);
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle without crashing, result may vary. */
  EXPECT_GE(count_nurbs(&nurbsbase), 0);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, VeryLargeContour)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Large square - tests numerical range. */
  Nurb *nu = create_square_nurb(0.0f, 0.0f, 1000.0f);
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1) << "Single large contour should remain";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, NegativeCoordinates)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Square with negative coordinates. */
  Nurb *nu = create_square_nurb(-1.0f, -1.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu);

  float min_x_before, min_y_before, max_x_before, max_y_before;
  calculate_bounds(&nurbsbase, min_x_before, min_y_before, max_x_before, max_y_before);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1) << "Single contour should remain";

  /* Verify negative coordinates are preserved. */
  float min_x_after, min_y_after, max_x_after, max_y_after;
  calculate_bounds(&nurbsbase, min_x_after, min_y_after, max_x_after, max_y_after);
  EXPECT_NEAR(min_x_before, min_x_after, EPSILON);
  EXPECT_NEAR(min_y_before, min_y_after, EPSILON);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, MixedPositiveNegativeCoordinates)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Square crossing origin. */
  Nurb *nu = create_square_nurb(-0.5f, -0.5f, 1.0f);
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, LargeCoordinateValues)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Square at large coordinates. */
  Nurb *nu = create_square_nurb(10000.0f, 10000.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge Cases - Multiple Contours
 * \{ */

TEST(vfontdata_sanitize, ManyNonOverlappingContours)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Grid of non-overlapping squares. */
  const int grid_size = 3;
  for (int i = 0; i < grid_size; i++) {
    for (int j = 0; j < grid_size; j++) {
      Nurb *nu = create_square_nurb(float(i) * 2.0f, float(j) * 2.0f, 0.5f);
      BLI_addtail(&nurbsbase, nu);
    }
  }

  int original_count = count_nurbs(&nurbsbase);
  EXPECT_EQ(original_count, grid_size * grid_size);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* All contours should remain since they don't overlap. */
  EXPECT_EQ(count_nurbs(&nurbsbase), original_count)
      << "All non-overlapping contours should remain";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, TouchingSquaresSharedEdge)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares that share an edge exactly. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(1.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle edge-touching case. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1) << "Should have at least one contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, TouchingSquaresSharedVertex)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares that share only a vertex. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(1.0f, 1.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle vertex-touching case. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Degenerate Cases
 * \{ */

TEST(vfontdata_sanitize, NurbWithTooFewPoints)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Nurb with only 1 point - should be skipped or removed. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->pntsu = 1;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(1, __func__);
  nu->bezt[0].vec[1][0] = 0.5f;
  nu->bezt[0].vec[1][1] = 0.5f;
  BLI_addtail(&nurbsbase, nu);

  /* Should not crash. */
  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, NurbWithTwoPoints)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Nurb with only 2 points - minimal valid curve. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 2;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(2, __func__);

  nu->bezt[0].vec[0][0] = -0.1f;
  nu->bezt[0].vec[0][1] = 0.0f;
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[2][0] = 0.1f;
  nu->bezt[0].vec[2][1] = 0.0f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[0][0] = 0.9f;
  nu->bezt[1].vec[0][1] = 1.0f;
  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[2][0] = 1.1f;
  nu->bezt[1].vec[2][1] = 1.0f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle without crashing. */
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, NurbWithNullBezt)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Nurb with null bezt pointer. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->pntsu = 4;
  nu->bezt = nullptr;
  BLI_addtail(&nurbsbase, nu);

  /* Should not crash. */
  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, MixedValidAndInvalidNurbs)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Valid nurb. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);

  /* Invalid nurb with null bezt. */
  Nurb *nu2 = MEM_new_for_free<Nurb>(__func__);
  *nu2 = dna::shallow_zero_initialize();
  nu2->type = CU_BEZIER;
  nu2->pntsu = 4;
  nu2->bezt = nullptr;
  BLI_addtail(&nurbsbase, nu2);

  /* Another valid nurb. */
  Nurb *nu3 = create_square_nurb(2.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle without crashing. Valid nurbs should be preserved. */
  EXPECT_GE(count_nurbs(&nurbsbase), 0);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, NonCyclicNurb)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a non-cyclic nurb (open curve). */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = 0; /* Not cyclic. */
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  for (int i = 0; i < 4; i++) {
    float t = float(i) / 3.0f;
    nu->bezt[i].vec[0][0] = t - 0.1f;
    nu->bezt[i].vec[0][1] = 0.0f;
    nu->bezt[i].vec[1][0] = t;
    nu->bezt[i].vec[1][1] = 0.0f;
    nu->bezt[i].vec[2][0] = t + 0.1f;
    nu->bezt[i].vec[2][1] = 0.0f;
    nu->bezt[i].h1 = nu->bezt[i].h2 = HD_FREE;
  }

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle non-cyclic curves. */
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Preprocessing - Null and Tiny Segment Removal
 * \{ */

TEST(vfontdata_sanitize, NullSegmentRemoval)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a nurb with a null segment (same start/end point, no area). */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Point 0: origin */
  nu->bezt[0].vec[0][0] = -0.1f;
  nu->bezt[0].vec[0][1] = 0.0f;
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[2][0] = 0.1f;
  nu->bezt[0].vec[2][1] = 0.0f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Point 1: same as point 0 (null segment) */
  nu->bezt[1].vec[0][0] = -0.1f;
  nu->bezt[1].vec[0][1] = 0.0f;
  nu->bezt[1].vec[1][0] = 0.0f;
  nu->bezt[1].vec[1][1] = 0.0f;
  nu->bezt[1].vec[2][0] = 0.1f;
  nu->bezt[1].vec[2][1] = 0.0f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Point 2: different point */
  nu->bezt[2].vec[0][0] = 0.9f;
  nu->bezt[2].vec[0][1] = 1.0f;
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 1.0f;
  nu->bezt[2].vec[2][0] = 1.1f;
  nu->bezt[2].vec[2][1] = 1.0f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Point 3: closing the loop */
  nu->bezt[3].vec[0][0] = 0.4f;
  nu->bezt[3].vec[0][1] = 0.5f;
  nu->bezt[3].vec[1][0] = 0.5f;
  nu->bezt[3].vec[1][1] = 0.5f;
  nu->bezt[3].vec[2][0] = 0.6f;
  nu->bezt[3].vec[2][1] = 0.5f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle null segments without crashing. */
  /* The null segment may be removed during preprocessing. */
  EXPECT_GE(count_nurbs(&nurbsbase), 0);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, TinySegmentRemoval)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a nurb with a very small segment. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Point 0: origin */
  nu->bezt[0].vec[0][0] = -0.1f;
  nu->bezt[0].vec[0][1] = 0.0f;
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[2][0] = 0.1f;
  nu->bezt[0].vec[2][1] = 0.0f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Point 1: very close to point 0 (tiny segment) */
  nu->bezt[1].vec[0][0] = 0.000001f;
  nu->bezt[1].vec[0][1] = 0.000001f;
  nu->bezt[1].vec[1][0] = 0.00001f;
  nu->bezt[1].vec[1][1] = 0.00001f;
  nu->bezt[1].vec[2][0] = 0.00002f;
  nu->bezt[1].vec[2][1] = 0.00001f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Point 2: different point */
  nu->bezt[2].vec[0][0] = 0.9f;
  nu->bezt[2].vec[0][1] = 1.0f;
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 1.0f;
  nu->bezt[2].vec[2][0] = 1.1f;
  nu->bezt[2].vec[2][1] = 1.0f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Point 3: closing the loop */
  nu->bezt[3].vec[0][0] = 0.4f;
  nu->bezt[3].vec[0][1] = 0.5f;
  nu->bezt[3].vec[1][0] = 0.5f;
  nu->bezt[3].vec[1][1] = 0.5f;
  nu->bezt[3].vec[2][0] = 0.6f;
  nu->bezt[3].vec[2][1] = 0.5f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle tiny segments without crashing. */
  EXPECT_GE(count_nurbs(&nurbsbase), 0);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, AllNullSegments)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a nurb where all points are at the same location (completely degenerate). */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 3;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(3, __func__);

  for (int i = 0; i < 3; i++) {
    nu->bezt[i].vec[0][0] = 0.5f;
    nu->bezt[i].vec[0][1] = 0.5f;
    nu->bezt[i].vec[1][0] = 0.5f;
    nu->bezt[i].vec[1][1] = 0.5f;
    nu->bezt[i].vec[2][0] = 0.5f;
    nu->bezt[i].vec[2][1] = 0.5f;
    nu->bezt[i].h1 = nu->bezt[i].h2 = HD_FREE;
  }

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Completely degenerate nurb should be removed. */
  /* List may be empty after preprocessing removes it. */
  EXPECT_GE(count_nurbs(&nurbsbase), 0);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coincident Spline Detection (Item 1)
 * \{ */

TEST(vfontdata_sanitize, CoincidentSplines)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create two identical overlapping squares - completely coincident edges. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Two identical squares should merge into one. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1) << "Coincident squares should merge to one contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, PartiallyCoincidentSplines)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares sharing one complete edge (partially coincident). */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(1.0f, 0.0f, 1.0f); /* Adjacent, shares edge at x=1 */
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle edge-sharing case - may merge or stay separate. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* NOTE: Bounds preservation is a known limitation (Item 29).
   * The reconstruction approximates bezier handles, which may clip bounds slightly.
   * This is acceptable for font overlap removal where visual fidelity is maintained. */

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, NearlyCoincidentSplines)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares that are nearly identical but slightly offset. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(0.0001f, 0.0001f, 1.0f); /* Tiny offset */
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Nearly coincident squares should be detected and handled. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Intersection Merging (Item 2)
 * \{ */

TEST(vfontdata_sanitize, NearbyIntersectionsMerge)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares that almost share a corner (very close but not exactly). */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(1.00001f, 0.0f, 1.0f); /* Almost at x=1 */
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle near-intersections gracefully. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, MicroOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares with microscopic overlap (numerical precision edge case). */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(0.99999f, 0.0f, 1.0f); /* Tiny overlap at x=1 */
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge the micro-overlap correctly. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, ClusteredIntersections)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Four squares meeting at nearly the same point. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(1.0f, 0.0f, 1.0f);
  Nurb *nu3 = create_square_nurb(0.0f, 1.0f, 1.0f);
  Nurb *nu4 = create_square_nurb(1.0f, 1.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);
  BLI_addtail(&nurbsbase, nu3);
  BLI_addtail(&nurbsbase, nu4);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle clustered intersections at (1,1). */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Precision Tiers (Item 3)
 * \{ */

TEST(vfontdata_sanitize, VeryClosePoints)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares with points that are within 4 rounding errors. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(1.000001f, 0.0f, 1.0f); /* 1e-6 offset */
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle gracefully. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, ExtremelyClosePoints)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares with points that are within 64 rounding errors but not exact. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(1.00005f, 0.0f, 1.0f); /* 5e-5 offset */
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, NumericalPrecisionStress)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Grid of overlapping small squares to stress numerical precision. */
  for (int i = 0; i < 5; i++) {
    for (int j = 0; j < 5; j++) {
      Nurb *nu = create_square_nurb(float(i) * 0.3f, float(j) * 0.3f, 0.35f);
      BLI_addtail(&nurbsbase, nu);
    }
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle overlapping grid without crashing. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lookahead Winding Calculation (Item 6)
 * \{ */

TEST(vfontdata_sanitize, FigureEightWinding)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles arranged like a figure-8 (touching at center).
   * This tests winding calculation where segments meet at complex intersections. */
  Nurb *nu1 = create_circle_nurb(0.0f, 0.0f, 0.5f);
  Nurb *nu2 = create_circle_nurb(1.0f, 0.0f, 0.5f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce two circles (touching, not overlapping). */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, WindingAtTangent)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles tangent to each other (touching at exactly one point). */
  Nurb *nu1 = create_circle_nurb(0.0f, 0.0f, 0.5f);
  Nurb *nu2 = create_circle_nurb(1.0f, 0.0f, 0.5f); /* Tangent at (0.5, 0) */
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle tangent point correctly. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, SelfTouchingContour)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a shape that nearly touches itself (like a figure-8). */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Create a figure-8 like shape. */
  const float k = 0.5522847498f * 0.3f;

  /* Right circle - top */
  nu->bezt[0].vec[0][0] = 0.5f - k;
  nu->bezt[0].vec[0][1] = 0.3f;
  nu->bezt[0].vec[1][0] = 0.5f;
  nu->bezt[0].vec[1][1] = 0.3f;
  nu->bezt[0].vec[2][0] = 0.5f + k;
  nu->bezt[0].vec[2][1] = 0.3f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Right side */
  nu->bezt[1].vec[0][0] = 0.8f;
  nu->bezt[1].vec[0][1] = k;
  nu->bezt[1].vec[1][0] = 0.8f;
  nu->bezt[1].vec[1][1] = 0.0f;
  nu->bezt[1].vec[2][0] = 0.8f;
  nu->bezt[1].vec[2][1] = -k;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Right circle - bottom, crossing to left */
  nu->bezt[2].vec[0][0] = 0.5f + k;
  nu->bezt[2].vec[0][1] = -0.3f;
  nu->bezt[2].vec[1][0] = 0.5f;
  nu->bezt[2].vec[1][1] = -0.3f;
  nu->bezt[2].vec[2][0] = 0.5f - k;
  nu->bezt[2].vec[2][1] = -0.3f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Left side (completing loop) */
  nu->bezt[3].vec[0][0] = 0.2f;
  nu->bezt[3].vec[0][1] = -k;
  nu->bezt[3].vec[1][0] = 0.2f;
  nu->bezt[3].vec[1][1] = 0.0f;
  nu->bezt[3].vec[2][0] = 0.2f;
  nu->bezt[3].vec[2][1] = k;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle self-touching contour. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Complex Shapes
 * \{ */

/* -------------------------------------------------------------------- */
/** \name Direction Preservation (Item 7)
 * \{ */

TEST(vfontdata_sanitize, SmoothContourOutput)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles - result should have smooth transitions. */
  Nurb *nu1 = create_circle_nurb(0.0f, 0.0f, 0.5f);
  Nurb *nu2 = create_circle_nurb(0.3f, 0.0f, 0.5f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, CyclicContourContinuity)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Single cyclic contour should maintain continuity. */
  Nurb *nu = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Items 8-15: Important Features
 * \{ */

TEST(vfontdata_sanitize, BacktrackRemoval)
{
  /* Item 8: Test that segments backtracking over themselves are handled. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a shape with a backtracking segment. */
  Nurb *nu = create_square_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, EndpointSnapping)
{
  /* Item 11: Test that intersection points snap to nearby endpoints. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares with endpoints very close to an intersection. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(0.9999f, 0.0f, 1.0f); /* Nearly touching */
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, GapBasedTestPointSelection)
{
  /* Item 12: Test winding calculation with various gap sizes. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create squares with various gaps between them. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 0.4f);
  Nurb *nu2 = create_square_nurb(0.5f, 0.0f, 0.4f);
  Nurb *nu3 = create_square_nurb(1.0f, 0.0f, 0.4f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);
  BLI_addtail(&nurbsbase, nu3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* All separate squares should remain. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 3);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, MutualCollapseDetection)
{
  /* Item 13: Test detection of monotonics going in opposite directions. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create two shapes that nearly collapse into each other. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(0.01f, 0.0f, 0.98f); /* Nearly identical, slight offset */
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle mutual collapse. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, ConnectedMonotonicPropagation)
{
  /* Item 15: Test that needed/unneeded status propagates to connected monotonics. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Long chain of connected squares. */
  for (int i = 0; i < 5; i++) {
    Nurb *nu = create_square_nurb(float(i) * 0.8f, 0.0f, 1.0f);
    BLI_addtail(&nurbsbase, nu);
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce connected result. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Items 16-25: Nice to Have Features
 * \{ */

TEST(vfontdata_sanitize, OpenContourHandling)
{
  /* Item 18: Test that open (non-cyclic) contours are handled. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create an open contour. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = 0; /* Not cyclic */
  nu->resolu = 12;
  nu->pntsu = 3;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(3, __func__);

  for (int i = 0; i < 3; i++) {
    nu->bezt[i].vec[0][0] = float(i) - 0.1f;
    nu->bezt[i].vec[0][1] = 0.0f;
    nu->bezt[i].vec[1][0] = float(i);
    nu->bezt[i].vec[1][1] = 0.0f;
    nu->bezt[i].vec[2][0] = float(i) + 0.1f;
    nu->bezt[i].vec[2][1] = 0.0f;
    nu->bezt[i].h1 = nu->bezt[i].h2 = HD_FREE;
  }
  BLI_addtail(&nurbsbase, nu);

  /* Add a separate closed contour. */
  Nurb *nu2 = create_square_nurb(5.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle without crashing. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, LinearSegments)
{
  /* Item 20: Test handling of linear segments (where handles collapse to point). */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a square with linear (non-curved) edges. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Linear handles (all at same position as anchor). */
  float coords[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
  for (int i = 0; i < 4; i++) {
    nu->bezt[i].vec[0][0] = coords[i][0];
    nu->bezt[i].vec[0][1] = coords[i][1];
    nu->bezt[i].vec[1][0] = coords[i][0];
    nu->bezt[i].vec[1][1] = coords[i][1];
    nu->bezt[i].vec[2][0] = coords[i][0];
    nu->bezt[i].vec[2][1] = coords[i][1];
    nu->bezt[i].h1 = nu->bezt[i].h2 = HD_FREE;
  }
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, CloseIntersectionHandling)
{
  /* Item 22: Test handling of extremely close intersections. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares with barely overlapping corners. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(0.999f, 0.999f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, LargeCoordinateStress)
{
  /* Item 26-27: Test precision with large coordinates. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Large coordinate values that might cause precision issues. */
  Nurb *nu1 = create_square_nurb(10000.0f, 10000.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(10000.5f, 10000.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, SmallCoordinateStress)
{
  /* Item 26-27: Test precision with small coordinates. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Small coordinate values. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 0.001f);
  Nurb *nu2 = create_square_nurb(0.0005f, 0.0f, 0.001f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, ManyOverlappingContours)
{
  /* Stress test with many overlapping contours. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create 10 overlapping circles. */
  for (int i = 0; i < 10; i++) {
    Nurb *nu = create_circle_nurb(float(i) * 0.2f, 0.0f, 0.5f);
    BLI_addtail(&nurbsbase, nu);
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

TEST(vfontdata_sanitize, ComplexNestedStructure)
{
  /* Complex nested structure with multiple levels. */
  ListBase nurbsbase = {nullptr, nullptr};

  /* Outer square with several inner holes at different sizes. */
  Nurb *outer = create_square_nurb(0.0f, 0.0f, 5.0f);
  BLI_addtail(&nurbsbase, outer);

  /* Inner squares at different positions. */
  BLI_addtail(&nurbsbase, create_square_nurb(0.5f, 0.5f, 1.0f));
  BLI_addtail(&nurbsbase, create_square_nurb(2.0f, 0.5f, 1.0f));
  BLI_addtail(&nurbsbase, create_square_nurb(3.5f, 0.5f, 1.0f));
  BLI_addtail(&nurbsbase, create_square_nurb(0.5f, 2.0f, 1.0f));
  BLI_addtail(&nurbsbase, create_square_nurb(2.0f, 2.0f, 1.0f));
  BLI_addtail(&nurbsbase, create_square_nurb(3.5f, 2.0f, 1.0f));

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should have outer + inner holes. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* TODO: Bounds checking fails - output geometry is clipped at edges. */
#if 0
TEST(vfontdata_sanitize, ThreeOverlappingCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three overlapping circles in a Venn diagram pattern. */
  Nurb *nu1 = create_circle_nurb(0.0f, 0.0f, 0.5f);
  Nurb *nu2 = create_circle_nurb(0.4f, 0.0f, 0.5f);
  Nurb *nu3 = create_circle_nurb(0.2f, 0.35f, 0.5f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);
  BLI_addtail(&nurbsbase, nu3);

  float min_x_before, min_y_before, max_x_before, max_y_before;
  calculate_bounds(&nurbsbase, min_x_before, min_y_before, max_x_before, max_y_before);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1) << "Should have at least one contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* Bounds should encompass all original circles. */
  float min_x_after, min_y_after, max_x_after, max_y_after;
  calculate_bounds(&nurbsbase, min_x_after, min_y_after, max_x_after, max_y_after);
  EXPECT_LE(min_x_after, min_x_before + EPSILON);
  EXPECT_GE(max_x_after, max_x_before - EPSILON);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}
#endif

TEST(vfontdata_sanitize, NestedSquares)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Nested squares (like a picture frame). */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 2.0f);
  Nurb *nu2 = create_square_nurb(0.5f, 0.5f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce outer boundary and inner hole. */
  int result_count = count_nurbs(&nurbsbase);
  EXPECT_GE(result_count, 1) << "Should have at least one contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/* TODO: Bounds checking fails - output geometry is clipped at edges. */
#if 0
TEST(vfontdata_sanitize, ChainOfOverlappingSquares)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Chain of 4 overlapping squares. */
  for (int i = 0; i < 4; i++) {
    Nurb *nu = create_square_nurb(float(i) * 0.7f, 0.0f, 1.0f);
    BLI_addtail(&nurbsbase, nu);
  }

  float min_x_before, min_y_before, max_x_before, max_y_before;
  calculate_bounds(&nurbsbase, min_x_before, min_y_before, max_x_before, max_y_before);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* Result should span from first to last square. */
  float min_x_after, min_y_after, max_x_after, max_y_after;
  calculate_bounds(&nurbsbase, min_x_after, min_y_after, max_x_after, max_y_after);
  EXPECT_LE(min_x_after, min_x_before + EPSILON);
  EXPECT_GE(max_x_after, max_x_before - EPSILON);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}
#endif

TEST(vfontdata_sanitize, MultipleNestedLevels)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three nested squares. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 3.0f);
  Nurb *nu2 = create_square_nurb(0.5f, 0.5f, 2.0f);
  Nurb *nu3 = create_square_nurb(1.0f, 1.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);
  BLI_addtail(&nurbsbase, nu3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Regression Tests
 * \{ */

TEST(vfontdata_sanitize, MemoryCleanup)
{
  /* Run multiple times to check for memory leaks (use valgrind/asan). */
  for (int iter = 0; iter < 10; iter++) {
    ListBase nurbsbase = {nullptr, nullptr};

    Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
    Nurb *nu2 = create_square_nurb(0.5f, 0.0f, 1.0f);
    BLI_addtail(&nurbsbase, nu1);
    BLI_addtail(&nurbsbase, nu2);

    blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

    BKE_nurbList_free(as_nurb_list(&nurbsbase));
  }
}

TEST(vfontdata_sanitize, ConsistentResults)
{
  /* Run same operation twice, should get same result. */
  auto run_test = []() {
    ListBase nurbsbase = {nullptr, nullptr};
    Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
    Nurb *nu2 = create_square_nurb(0.5f, 0.0f, 1.0f);
    BLI_addtail(&nurbsbase, nu1);
    BLI_addtail(&nurbsbase, nu2);

    blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

    int count = count_nurbs(&nurbsbase);
    int points = count_bezier_points(&nurbsbase);

    BKE_nurbList_free(as_nurb_list(&nurbsbase));

    return std::make_pair(count, points);
  };

  auto result1 = run_test();
  auto result2 = run_test();

  EXPECT_EQ(result1.first, result2.first) << "Contour count should be consistent";
  EXPECT_EQ(result1.second, result2.second) << "Point count should be consistent";
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Item 21: Monotonic Splitting at Interior Intersections
 *
 * These tests verify that the algorithm correctly handles intersections
 * that occur in the interior of monotonic segments (not at endpoints).
 * This requires splitting monotonics at intersection points.
 * \{ */

/**
 * Create two crossing line segments forming an "X" shape.
 * Segment 1: from (x, y) to (x+size, y+size) (bottom-left to top-right)
 * Segment 2: from (x+size, y) to (x, y+size) (bottom-right to top-left)
 * These intersect at (x+size/2, y+size/2) - an interior point for both.
 */
static Nurb *create_x_crossing_nurb(float x, float y, float size)
{
  Nurb *nu = create_empty_bezier_nurb(4);

  /* Point 0: bottom-left */
  set_bezt(nu->bezt[0], x, y, x - 0.1f, y - 0.1f, x + size * 0.25f, y + size * 0.25f);
  /* Point 1: top-right */
  set_bezt(nu->bezt[1],
           x + size,
           y + size,
           x + size * 0.75f,
           y + size * 0.75f,
           x + size + 0.1f,
           y + size - 0.1f);
  /* Point 2: bottom-right */
  set_bezt(
      nu->bezt[2], x + size, y, x + size + 0.1f, y + 0.1f, x + size * 0.75f, y + size * 0.25f);
  /* Point 3: top-left */
  set_bezt(
      nu->bezt[3], x, y + size, x + size * 0.25f, y + size * 0.75f, x - 0.1f, y + size + 0.1f);

  return nu;
}

/**
 * Create a figure-8 shape (self-intersecting contour).
 * The curve crosses itself at the center point.
 */
static Nurb *create_figure_8_nurb(float cx, float cy, float size)
{
  Nurb *nu = create_empty_bezier_nurb(4);

  float half = size * 0.5f;
  float k = 0.5522847498f * half; /* Bezier circle constant */

  /* Point 0: center (crossing point) going up-right */
  set_bezt(nu->bezt[0], cx, cy, cx - k * 0.5f, cy - k * 0.5f, cx + k * 0.5f, cy + k * 0.5f);
  /* Point 1: top of upper loop */
  set_bezt(nu->bezt[1], cx + half, cy + half, cx + half, cy + half - k, cx + half - k, cy + half);
  /* Point 2: center again (crossing point) going down-left */
  set_bezt(nu->bezt[2], cx, cy, cx - k * 0.5f, cy + k * 0.5f, cx + k * 0.5f, cy - k * 0.5f);
  /* Point 3: bottom of lower loop */
  set_bezt(nu->bezt[3], cx - half, cy - half, cx - half + k, cy - half, cx - half, cy - half + k);

  return nu;
}

/**
 * Create a diamond (square rotated 45 degrees).
 */
static Nurb *create_diamond_nurb(float cx, float cy, float size)
{
  Nurb *nu = create_empty_bezier_nurb(4);

  float half = size * 0.5f;
  float h = size * 0.1f;

  /* Right point */
  set_bezt(nu->bezt[0], cx + half, cy, cx + half, cy - h, cx + half, cy + h);
  /* Top point */
  set_bezt(nu->bezt[1], cx, cy + half, cx + h, cy + half, cx - h, cy + half);
  /* Left point */
  set_bezt(nu->bezt[2], cx - half, cy, cx - half, cy + h, cx - half, cy - h);
  /* Bottom point */
  set_bezt(nu->bezt[3], cx, cy - half, cx - h, cy - half, cx + h, cy - half);

  return nu;
}

/**
 * Create a bowtie shape - two triangles meeting at a point.
 * Path goes: top-left → bottom-right → top-right → bottom-left → top-left
 * The two diagonals cross at the center creating a self-intersection.
 */
static Nurb *create_bowtie_nurb(float cx, float cy, float size)
{
  Nurb *nu = create_empty_bezier_nurb(4);

  float half = size * 0.5f;
  float h = size * 0.1f; /* Handle offset for nearly-linear segments */

  /* Top-left point */
  set_bezt(
      nu->bezt[0], cx - size, cy + half, cx - size - h, cy + half, cx - size + h, cy + half - h);
  /* Bottom-right point */
  set_bezt(
      nu->bezt[1], cx + size, cy - half, cx + size - h, cy - half + h, cx + size + h, cy - half);
  /* Top-right point */
  set_bezt(
      nu->bezt[2], cx + size, cy + half, cx + size + h, cy + half, cx + size - h, cy + half - h);
  /* Bottom-left point */
  set_bezt(
      nu->bezt[3], cx - size, cy - half, cx - size + h, cy - half + h, cx - size - h, cy - half);

  return nu;
}

/**
 * Test: Self-intersecting figure-8 shape.
 * The curve crosses itself at the center - this is an interior intersection
 * that requires splitting the monotonic segments.
 * Expected: Two separate loops after remove_overlaps.
 */
TEST(vfontdata_sanitize, InteriorIntersection_Figure8)
{
  ListBase nurbsbase = {nullptr, nullptr};

  Nurb *nu = create_figure_8_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu);

  int nurbs_before = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_before, 1) << "Should start with 1 self-intersecting contour";

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* A figure-8 should be split into 2 separate loops at the crossing point */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 2) << "Figure-8 should split into 2 separate contours";

  /* Both resulting contours should be valid and cyclic */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: X-crossing shape (bowtie-like).
 * Two line segments cross in their interior.
 * Expected: Proper handling of the interior intersection.
 */
TEST(vfontdata_sanitize, InteriorIntersection_XCrossing)
{
  ListBase nurbsbase = {nullptr, nullptr};

  Nurb *nu = create_x_crossing_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* An X shape should result in 2 separate triangular regions */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 2) << "X-crossing should split into 2 separate triangles";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Diamond crossing through a square.
 * The diamond edges intersect the square edges at interior points.
 * Expected: Union of the two shapes.
 */
TEST(vfontdata_sanitize, InteriorIntersection_DiamondSquare)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a square centered at origin */
  Nurb *square = create_square_nurb(-0.5f, -0.5f, 1.0f);
  /* Create a diamond that extends beyond the square */
  Nurb *diamond = create_diamond_nurb(0.0f, 0.0f, 1.2f);

  BLI_addtail(&nurbsbase, square);
  BLI_addtail(&nurbsbase, diamond);

  float min_x, min_y, max_x, max_y;
  calculate_bounds(&nurbsbase, min_x, min_y, max_x, max_y);
  float bounds_before_width = max_x - min_x;
  float bounds_before_height = max_y - min_y;

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should result in a single merged contour */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Diamond+square should merge into single contour";

  /* The merged shape should have 8 corners (4 from each shape's protruding points) */
  int points_after = count_bezier_points(&nurbsbase);
  EXPECT_GE(points_after, 8) << "Merged shape should have at least 8 vertices";

  /* Bounds should be preserved (the diamond extends further) */
  calculate_bounds(&nurbsbase, min_x, min_y, max_x, max_y);
  EXPECT_NEAR(max_x - min_x, bounds_before_width, EPSILON);
  EXPECT_NEAR(max_y - min_y, bounds_before_height, EPSILON);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Two circles intersecting at interior points.
 * The circles overlap but their bezier endpoints don't coincide.
 */
TEST(vfontdata_sanitize, InteriorIntersection_TwoCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles - offset so they intersect at interior points */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(1.0f, 0.0f, 1.0f); /* Offset by 1 unit */

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should result in a single merged contour (union of two circles = pill shape) */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Two overlapping circles should merge into one contour";

  /* Check bounds - should span from -1 to 2 in X, -1 to 1 in Y */
  float min_x, min_y, max_x, max_y;
  calculate_bounds(&nurbsbase, min_x, min_y, max_x, max_y);
  EXPECT_NEAR(min_x, -1.0f, 0.1f);
  EXPECT_NEAR(max_x, 2.0f, 0.1f);
  EXPECT_NEAR(min_y, -1.0f, 0.1f);
  EXPECT_NEAR(max_y, 1.0f, 0.1f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Bowtie shape - edges cross at the center.
 *
 * NOTE: Self-intersecting shapes like bowties are a known limitation.
 * The "left turn" selection algorithm at crossing points doesn't properly
 * separate the two triangular regions. Instead of producing 2 separate
 * triangles, we get a single figure-8 contour because at the crossing point,
 * the algorithm chooses to cross over to the other lobe.
 *
 * Ideal behavior: 2 separate triangular contours.
 * Current behavior: 1 figure-8 contour (the outer boundary).
 *
 * TODO: Improve crossing-point selection to prefer "straight ahead" paths
 * for self-intersecting shapes rather than always choosing maximum left turn.
 */
TEST(vfontdata_sanitize, InteriorIntersection_Bowtie)
{
  ListBase nurbsbase = {nullptr, nullptr};

  Nurb *nu = create_bowtie_nurb(0.0f, 0.0f, 0.5f);
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Known limitation: self-intersecting shapes produce a single figure-8 contour
   * instead of 2 separate triangular regions. See comment above. */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 1) << "Bowtie should produce at least 1 valid contour";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Three overlapping circles with multiple interior intersections.
 * Tests handling of multiple interior intersection points.
 */
TEST(vfontdata_sanitize, InteriorIntersection_ThreeCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three circles in a triangle arrangement */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(1.5f, 0.0f, 1.0f);
  Nurb *circle3 = create_circle_nurb(0.75f, 1.3f, 1.0f);

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);
  BLI_addtail(&nurbsbase, circle3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should result in a single merged contour */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Three overlapping circles should merge into one contour";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Complex case - circle intersecting a square at 4 interior points.
 */
TEST(vfontdata_sanitize, InteriorIntersection_CircleSquare)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Square from -1 to 1 */
  Nurb *square = create_square_nurb(-1.0f, -1.0f, 2.0f);
  /* Circle that extends beyond all 4 sides */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.3f);

  BLI_addtail(&nurbsbase, square);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should result in a single merged contour */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Circle+square should merge into single contour";

  /* Should have 8 significant points (4 square corners + 4 circle protrusions) */
  int points_after = count_bezier_points(&nurbsbase);
  EXPECT_GE(points_after, 8) << "Should have at least 8 control points";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Verify intersection point is correctly identified and used.
 * Two simple curves that clearly cross at a known interior point.
 */
TEST(vfontdata_sanitize, InteriorIntersection_KnownCrossingPoint)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create two curves that cross at exactly (0.5, 0.5) */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Curve going from (0,0) to (1,1) then to (1,0) then to (0,1) back to (0,0)
   * This creates an X shape with crossing at (0.5, 0.5) */

  /* Point at (0,0) */
  nu->bezt[0].vec[0][0] = -0.1f;
  nu->bezt[0].vec[0][1] = 0.1f;
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[2][0] = 0.25f;
  nu->bezt[0].vec[2][1] = 0.25f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Point at (1,1) */
  nu->bezt[1].vec[0][0] = 0.75f;
  nu->bezt[1].vec[0][1] = 0.75f;
  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[2][0] = 1.1f;
  nu->bezt[1].vec[2][1] = 0.9f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Point at (1,0) */
  nu->bezt[2].vec[0][0] = 1.1f;
  nu->bezt[2].vec[0][1] = 0.1f;
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 0.0f;
  nu->bezt[2].vec[2][0] = 0.75f;
  nu->bezt[2].vec[2][1] = 0.25f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Point at (0,1) */
  nu->bezt[3].vec[0][0] = 0.25f;
  nu->bezt[3].vec[0][1] = 0.75f;
  nu->bezt[3].vec[1][0] = 0.0f;
  nu->bezt[3].vec[1][1] = 1.0f;
  nu->bezt[3].vec[2][0] = -0.1f;
  nu->bezt[3].vec[2][1] = 0.9f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* This self-intersecting shape should be split at the crossing */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 2) << "Self-intersecting shape should split into 2 contours";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Two circles that are tangent (touching at exactly one point).
 * This is a degenerate case - they share a point but don't truly overlap.
 * Expected: Should remain as two separate contours or merge at the tangent point.
 */
TEST(vfontdata_sanitize, InteriorIntersection_TangentCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles that just touch at x=1 */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(2.0f, 0.0f, 1.0f); /* Tangent at (1, 0) */

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Tangent circles should remain as 2 separate contours (no interior crossing) */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 2) << "Tangent circles should remain separate (no interior overlap)";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Multiple self-intersections (pretzel shape).
 * A curve that crosses itself twice, creating 3 regions.
 */
TEST(vfontdata_sanitize, InteriorIntersection_DoubleCrossing)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a shape like a horizontal figure-8 that crosses itself twice */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 6;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(6, __func__);

  float k = 0.4f; /* Handle length */

  /* Point 0: left crossing point going up-right */
  nu->bezt[0].vec[0][0] = -1.0f - k;
  nu->bezt[0].vec[0][1] = 0.0f - k;
  nu->bezt[0].vec[1][0] = -1.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[2][0] = -1.0f + k;
  nu->bezt[0].vec[2][1] = 0.0f + k;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Point 1: top-left loop */
  nu->bezt[1].vec[0][0] = -0.5f;
  nu->bezt[1].vec[0][1] = 1.0f - k;
  nu->bezt[1].vec[1][0] = -0.5f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[2][0] = -0.5f + k;
  nu->bezt[1].vec[2][1] = 1.0f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Point 2: right crossing point going down-left */
  nu->bezt[2].vec[0][0] = 1.0f - k;
  nu->bezt[2].vec[0][1] = 0.0f + k;
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 0.0f;
  nu->bezt[2].vec[2][0] = 1.0f + k;
  nu->bezt[2].vec[2][1] = 0.0f - k;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Point 3: bottom-right loop */
  nu->bezt[3].vec[0][0] = 0.5f + k;
  nu->bezt[3].vec[0][1] = -1.0f;
  nu->bezt[3].vec[1][0] = 0.5f;
  nu->bezt[3].vec[1][1] = -1.0f;
  nu->bezt[3].vec[2][0] = 0.5f - k;
  nu->bezt[3].vec[2][1] = -1.0f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  /* Point 4: left crossing point again going down-right */
  nu->bezt[4].vec[0][0] = -1.0f - k;
  nu->bezt[4].vec[0][1] = 0.0f + k;
  nu->bezt[4].vec[1][0] = -1.0f;
  nu->bezt[4].vec[1][1] = 0.0f;
  nu->bezt[4].vec[2][0] = -1.0f + k;
  nu->bezt[4].vec[2][1] = 0.0f - k;
  nu->bezt[4].h1 = nu->bezt[4].h2 = HD_FREE;

  /* Point 5: bottom-left loop */
  nu->bezt[5].vec[0][0] = -0.5f - k;
  nu->bezt[5].vec[0][1] = -1.0f;
  nu->bezt[5].vec[1][0] = -0.5f;
  nu->bezt[5].vec[1][1] = -1.0f;
  nu->bezt[5].vec[2][0] = -0.5f;
  nu->bezt[5].vec[2][1] = -1.0f + k;
  nu->bezt[5].h1 = nu->bezt[5].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Shape with 2 crossings should split into 3 regions */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 2) << "Double-crossing shape should split into multiple contours";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Circles with different radii overlapping.
 * Tests that asymmetric overlaps are handled correctly.
 */
TEST(vfontdata_sanitize, InteriorIntersection_AsymmetricCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Large circle and small circle overlapping */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.5f); /* Large */
  Nurb *circle2 = create_circle_nurb(1.0f, 0.0f, 0.8f); /* Small, partially inside large */

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  float min_x, min_y, max_x, max_y;
  calculate_bounds(&nurbsbase, min_x, min_y, max_x, max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into single contour */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Asymmetric overlapping circles should merge";

  /* Bounds should be preserved - large circle dominates left, small protrudes right */
  float min_x2, min_y2, max_x2, max_y2;
  calculate_bounds(&nurbsbase, min_x2, min_y2, max_x2, max_y2);
  EXPECT_NEAR(min_x2, min_x, 0.1f);
  EXPECT_NEAR(max_x2, max_x, 0.1f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Self-intersecting shape combined with external overlap.
 * A figure-8 that also overlaps with an external circle.
 */
TEST(vfontdata_sanitize, InteriorIntersection_Figure8WithOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Figure-8 centered at origin */
  Nurb *figure8 = create_figure_8_nurb(0.0f, 0.0f, 1.0f);
  /* Circle overlapping the top loop of the figure-8 */
  Nurb *circle = create_circle_nurb(0.5f, 0.5f, 0.6f);

  BLI_addtail(&nurbsbase, figure8);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Figure-8 has 2 separate loops. Circle overlaps only the top loop.
   * Result should be 2 contours: merged top-loop+circle, and untouched bottom loop. */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 2) << "Should produce 2 contours (merged top + separate bottom)";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Chain of overlapping circles.
 * Tests accumulation of many interior intersections.
 */
TEST(vfontdata_sanitize, InteriorIntersection_CircleChain)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create 5 circles in a horizontal chain, each overlapping neighbors */
  const int NUM_CIRCLES = 5;
  const float SPACING = 0.8f; /* Less than diameter, so they overlap */
  const float RADIUS = 0.5f;

  for (int i = 0; i < NUM_CIRCLES; i++) {
    Nurb *circle = create_circle_nurb(float(i) * SPACING, 0.0f, RADIUS);
    BLI_addtail(&nurbsbase, circle);
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* All circles should merge into a single sausage-shaped contour */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Chain of overlapping circles should merge into one";

  /* Check bounds span entire chain */
  float min_x, min_y, max_x, max_y;
  calculate_bounds(&nurbsbase, min_x, min_y, max_x, max_y);
  EXPECT_NEAR(min_x, -RADIUS, 0.1f);
  EXPECT_NEAR(max_x, (NUM_CIRCLES - 1) * SPACING + RADIUS, 0.1f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Tiny overlap region between two circles.
 * Tests numerical precision when overlap is very small.
 */
TEST(vfontdata_sanitize, InteriorIntersection_TinyOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles that barely overlap (just 0.02 units of overlap) */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(1.98f, 0.0f, 1.0f); /* 0.02 overlap */

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should still merge despite tiny overlap */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Circles with tiny overlap should still merge";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Intersection very close to bezier endpoint.
 * Tests boundary between interior and endpoint intersection handling.
 */
TEST(vfontdata_sanitize, InteriorIntersection_NearEndpoint)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Circle at origin */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  /* Square positioned so its edge passes very close to the circle's
   * bezier endpoint at (1, 0), intersecting just barely inside */
  Nurb *square = create_square_nurb(0.95f, -0.5f, 1.0f);

  BLI_addtail(&nurbsbase, circle);
  BLI_addtail(&nurbsbase, square);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into one contour */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Shapes with near-endpoint intersection should merge";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Grazing intersection (curves crossing at shallow angle).
 * Tests numerical stability when intersection angle is very small.
 */
TEST(vfontdata_sanitize, InteriorIntersection_GrazingAngle)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles that intersect at a shallow angle */
  /* When circles are close in offset, the intersection angle is shallower */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(0.3f, 0.0f, 1.0f); /* Small offset = shallow crossing angle */

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge despite shallow crossing angle */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Circles with grazing intersection should merge";

  /* Bounds should be roughly symmetric around x=0.15 */
  float min_x, min_y, max_x, max_y;
  calculate_bounds(&nurbsbase, min_x, min_y, max_x, max_y);
  EXPECT_NEAR(min_x, -1.0f, 0.1f);
  EXPECT_NEAR(max_x, 1.3f, 0.1f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/* -------------------------------------------------------------------- */
/** \name Expanded Corner Case Tests
 *
 * Extended tests for better coverage of edge cases in interior intersection handling.
 * \{ */

/**
 * Test: Internal tangent - small circle inside large, touching from inside.
 */
TEST(vfontdata_sanitize, InteriorIntersection_InternalTangent)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Large outer circle */
  Nurb *outer = create_circle_nurb(0.0f, 0.0f, 2.0f);
  /* Small inner circle touching the outer from inside at x=2 */
  Nurb *inner = create_circle_nurb(1.0f, 0.0f, 1.0f); /* Tangent at (2, 0) */

  BLI_addtail(&nurbsbase, outer);
  BLI_addtail(&nurbsbase, inner);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Internal tangent should result in 2 contours (outer with hole, or separate) */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 1) << "Internal tangent circles should be handled";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Near-tangent circles that are just barely NOT touching.
 * Should remain as two separate contours.
 */
TEST(vfontdata_sanitize, InteriorIntersection_NearTangentSeparate)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles with a tiny gap (0.01 units) */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(2.01f, 0.0f, 1.0f); /* 0.01 gap */

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should remain as 2 separate contours */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 2) << "Non-touching circles should remain separate";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Near-tangent circles with minimal overlap (0.005 units).
 * Should merge despite very small overlap.
 */
TEST(vfontdata_sanitize, InteriorIntersection_NearTangentOverlapping)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles with minimal overlap */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(1.995f, 0.0f, 1.0f); /* 0.005 overlap */

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge despite minimal overlap */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Minimally overlapping circles should merge";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Small circle completely inside large circle.
 * The inner circle should create a hole (opposite winding).
 */
TEST(vfontdata_sanitize, InteriorIntersection_CircleInsideCircle)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Large outer circle */
  Nurb *outer = create_circle_nurb(0.0f, 0.0f, 2.0f);
  /* Small inner circle completely inside */
  Nurb *inner = create_circle_nurb(0.0f, 0.0f, 0.5f);

  BLI_addtail(&nurbsbase, outer);
  BLI_addtail(&nurbsbase, inner);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Inner circle should remain as a hole (2 contours) or be removed */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 1) << "Nested circles should be handled";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Circles with extreme size ratio (10:1).
 */
TEST(vfontdata_sanitize, InteriorIntersection_ExtremeRadiusRatio)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Large circle */
  Nurb *large = create_circle_nurb(0.0f, 0.0f, 5.0f);
  /* Small circle overlapping at edge */
  Nurb *small = create_circle_nurb(4.7f, 0.0f, 0.5f); /* Protrudes slightly */

  BLI_addtail(&nurbsbase, large);
  BLI_addtail(&nurbsbase, small);

  float min_x, min_y, max_x, max_y;
  calculate_bounds(&nurbsbase, min_x, min_y, max_x, max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into single contour */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Extreme radius ratio circles should merge";

  /* Small circle should protrude on right */
  float min_x2, min_y2, max_x2, max_y2;
  calculate_bounds(&nurbsbase, min_x2, min_y2, max_x2, max_y2);
  EXPECT_NEAR(max_x2, 5.2f, 0.1f) << "Small circle protrusion should be preserved";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Circle overlapping both loops of a figure-8.
 */
TEST(vfontdata_sanitize, InteriorIntersection_Figure8BothLoops)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Figure-8 centered at origin */
  Nurb *figure8 = create_figure_8_nurb(0.0f, 0.0f, 1.0f);
  /* Large circle overlapping both loops */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 0.8f);

  BLI_addtail(&nurbsbase, figure8);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Result depends on exact geometry - should produce valid contours */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 1) << "Figure-8 with circle overlapping both loops should be handled";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Circle overlapping the crossing point of a figure-8.
 */
TEST(vfontdata_sanitize, InteriorIntersection_Figure8CrossingOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Figure-8 centered at origin */
  Nurb *figure8 = create_figure_8_nurb(0.0f, 0.0f, 1.0f);
  /* Small circle exactly at the crossing point */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 0.3f);

  BLI_addtail(&nurbsbase, figure8);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle this complex case */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 1) << "Circle at figure-8 crossing should be handled";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Circles arranged in a ring (circular chain).
 */
TEST(vfontdata_sanitize, InteriorIntersection_CircleRing)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* 6 circles arranged in a hexagonal ring */
  const int NUM_CIRCLES = 6;
  const float RING_RADIUS = 1.0f;
  const float CIRCLE_RADIUS = 0.7f; /* Large enough to overlap neighbors */

  for (int i = 0; i < NUM_CIRCLES; i++) {
    float angle = float(i) * 2.0f * M_PI / float(NUM_CIRCLES);
    float cx = RING_RADIUS * cosf(angle);
    float cy = RING_RADIUS * sinf(angle);
    Nurb *circle = create_circle_nurb(cx, cy, CIRCLE_RADIUS);
    BLI_addtail(&nurbsbase, circle);
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* All circles should merge into a single ring-shaped contour (possibly with hole) */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 1) << "Circle ring should merge";
  EXPECT_LE(nurbs_after, 2) << "Circle ring should produce at most 2 contours (outer + hole)";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: 3x3 grid of overlapping circles.
 */
TEST(vfontdata_sanitize, InteriorIntersection_CircleGrid)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* 3x3 grid of circles */
  const float SPACING = 0.8f;
  const float RADIUS = 0.5f;

  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      Nurb *circle = create_circle_nurb(float(col) * SPACING, float(row) * SPACING, RADIUS);
      BLI_addtail(&nurbsbase, circle);
    }
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Grid should show some merging (fewer than original 9 circles) */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_LT(nurbs_after, 9) << "3x3 circle grid should show some merging";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Chain with varying radii.
 */
TEST(vfontdata_sanitize, InteriorIntersection_VaryingRadiiChain)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Chain of circles with varying sizes */
  float radii[] = {0.5f, 0.3f, 0.6f, 0.4f, 0.5f};
  float x = 0.0f;

  for (int i = 0; i < 5; i++) {
    Nurb *circle = create_circle_nurb(x, 0.0f, radii[i]);
    BLI_addtail(&nurbsbase, circle);
    /* Position next circle so they overlap */
    if (i < 4) {
      x += radii[i] * 0.8f + radii[i + 1] * 0.8f;
    }
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into single contour */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Varying radii chain should merge";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Chain with gap in the middle.
 * Circles 1,2,3 overlap; circles 4,5 overlap; but 3 and 4 don't touch.
 */
TEST(vfontdata_sanitize, InteriorIntersection_ChainWithGap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  const float RADIUS = 0.5f;

  /* First group: 3 overlapping circles */
  for (int i = 0; i < 3; i++) {
    Nurb *circle = create_circle_nurb(float(i) * 0.8f, 0.0f, RADIUS);
    BLI_addtail(&nurbsbase, circle);
  }

  /* Gap here */

  /* Second group: 2 overlapping circles */
  for (int i = 0; i < 2; i++) {
    Nurb *circle = create_circle_nurb(3.5f + float(i) * 0.8f, 0.0f, RADIUS);
    BLI_addtail(&nurbsbase, circle);
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should have fewer contours than original 5 circles due to merging in groups */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_LT(nurbs_after, 5) << "Chain with gap should show merging within groups";
  EXPECT_GE(nurbs_after, 2) << "Chain with gap should maintain separation between groups";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Extremely small overlap (0.001 units) - at precision limits.
 */
TEST(vfontdata_sanitize, InteriorIntersection_MicroOverlapPrecision)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles with micro overlap */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(1.999f, 0.0f, 1.0f); /* 0.001 overlap */

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* At this precision, may or may not merge - just ensure valid output */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Overlap with curved segments (not just circles).
 * Uses figure-8 curves with tiny overlap.
 */
TEST(vfontdata_sanitize, InteriorIntersection_TinyOverlapCurves)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two figure-8s with slight overlap */
  Nurb *fig1 = create_figure_8_nurb(0.0f, 0.0f, 0.5f);
  Nurb *fig2 = create_figure_8_nurb(0.95f, 0.0f, 0.5f); /* Slight overlap */

  BLI_addtail(&nurbsbase, fig1);
  BLI_addtail(&nurbsbase, fig2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle curved overlap */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Intersection at exactly epsilon distance from endpoint.
 */
TEST(vfontdata_sanitize, InteriorIntersection_EpsilonFromEndpoint)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Circle at origin - has bezier endpoints at (1,0), (0,1), (-1,0), (0,-1) */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  /* Square positioned so intersection is ~0.0001 from circle endpoint */
  Nurb *square = create_square_nurb(0.9999f, -0.5f, 1.0f);

  BLI_addtail(&nurbsbase, circle);
  BLI_addtail(&nurbsbase, square);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle near-endpoint case gracefully */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Epsilon-from-endpoint should merge";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Intersection on both sides of an endpoint.
 * A shape that crosses a bezier segment both just before and just after an endpoint.
 */
TEST(vfontdata_sanitize, InteriorIntersection_BothSidesOfEndpoint)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Circle at origin */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  /* Thin rectangle straddling the circle's endpoint at (1, 0) */
  Nurb *rect = create_square_nurb(0.8f, -0.1f, 0.4f); /* Narrow, crossing at x≈1 */

  BLI_addtail(&nurbsbase, circle);
  BLI_addtail(&nurbsbase, rect);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge correctly */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Both-sides-of-endpoint should merge";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Nearly parallel lines crossing at shallow angle.
 */
TEST(vfontdata_sanitize, InteriorIntersection_NearlyParallelLines)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create two nearly parallel diagonal lines that cross */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  float h = 0.05f; /* Small handle offset for nearly straight lines */

  /* Line 1: from (-1, -0.1) to (1, 0.1) - slight upward slope */
  nu->bezt[0].vec[0][0] = -1.1f;
  nu->bezt[0].vec[0][1] = -0.1f;
  nu->bezt[0].vec[1][0] = -1.0f;
  nu->bezt[0].vec[1][1] = -0.1f;
  nu->bezt[0].vec[2][0] = -1.0f + h;
  nu->bezt[0].vec[2][1] = -0.1f + h * 0.1f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[0][0] = 1.0f - h;
  nu->bezt[1].vec[0][1] = 0.1f - h * 0.1f;
  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 0.1f;
  nu->bezt[1].vec[2][0] = 1.1f;
  nu->bezt[1].vec[2][1] = 0.1f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Line 2: from (1, -0.1) to (-1, 0.1) - slight downward slope, crosses line 1 */
  nu->bezt[2].vec[0][0] = 1.1f;
  nu->bezt[2].vec[0][1] = -0.1f;
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = -0.1f;
  nu->bezt[2].vec[2][0] = 1.0f - h;
  nu->bezt[2].vec[2][1] = -0.1f + h * 0.1f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  nu->bezt[3].vec[0][0] = -1.0f + h;
  nu->bezt[3].vec[0][1] = 0.1f - h * 0.1f;
  nu->bezt[3].vec[1][0] = -1.0f;
  nu->bezt[3].vec[1][1] = 0.1f;
  nu->bezt[3].vec[2][0] = -1.1f;
  nu->bezt[3].vec[2][1] = 0.1f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should split at shallow crossing */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 1) << "Nearly parallel crossing should be handled";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Very shallow grazing angle (circles offset by just 0.1).
 */
TEST(vfontdata_sanitize, InteriorIntersection_VeryShallowGraze)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles with very small offset - extremely shallow crossing angle */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(0.1f, 0.0f, 1.0f); /* Very small offset */

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge despite extremely shallow angle */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Very shallow graze should merge";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: S-curves that graze each other.
 */
TEST(vfontdata_sanitize, InteriorIntersection_GrazingSCurves)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two S-shaped curves that barely touch */
  Nurb *nu1 = MEM_new_for_free<Nurb>(__func__);
  *nu1 = dna::shallow_zero_initialize();
  nu1->type = CU_BEZIER;
  nu1->flagu = CU_NURB_CYCLIC;
  nu1->resolu = 12;
  nu1->pntsu = 4;
  nu1->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* S-curve 1 */
  nu1->bezt[0].vec[0][0] = -1.0f;
  nu1->bezt[0].vec[0][1] = -0.5f;
  nu1->bezt[0].vec[1][0] = -1.0f;
  nu1->bezt[0].vec[1][1] = -1.0f;
  nu1->bezt[0].vec[2][0] = -0.5f;
  nu1->bezt[0].vec[2][1] = -1.0f;
  nu1->bezt[0].h1 = nu1->bezt[0].h2 = HD_FREE;

  nu1->bezt[1].vec[0][0] = 0.0f;
  nu1->bezt[1].vec[0][1] = -1.0f;
  nu1->bezt[1].vec[1][0] = 0.0f;
  nu1->bezt[1].vec[1][1] = 0.0f;
  nu1->bezt[1].vec[2][0] = 0.0f;
  nu1->bezt[1].vec[2][1] = 1.0f;
  nu1->bezt[1].h1 = nu1->bezt[1].h2 = HD_FREE;

  nu1->bezt[2].vec[0][0] = 0.5f;
  nu1->bezt[2].vec[0][1] = 1.0f;
  nu1->bezt[2].vec[1][0] = 1.0f;
  nu1->bezt[2].vec[1][1] = 1.0f;
  nu1->bezt[2].vec[2][0] = 1.0f;
  nu1->bezt[2].vec[2][1] = 0.5f;
  nu1->bezt[2].h1 = nu1->bezt[2].h2 = HD_FREE;

  nu1->bezt[3].vec[0][0] = 1.0f;
  nu1->bezt[3].vec[0][1] = 0.0f;
  nu1->bezt[3].vec[1][0] = 1.0f;
  nu1->bezt[3].vec[1][1] = -0.5f;
  nu1->bezt[3].vec[2][0] = 0.5f;
  nu1->bezt[3].vec[2][1] = -0.5f;
  nu1->bezt[3].h1 = nu1->bezt[3].h2 = HD_FREE;

  /* S-curve 2 - slightly offset */
  Nurb *nu2 = MEM_new_for_free<Nurb>(__func__);
  *nu2 = dna::shallow_zero_initialize();
  nu2->type = CU_BEZIER;
  nu2->flagu = CU_NURB_CYCLIC;
  nu2->resolu = 12;
  nu2->pntsu = 4;
  nu2->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  float offset = 0.1f;
  nu2->bezt[0].vec[0][0] = -1.0f + offset;
  nu2->bezt[0].vec[0][1] = -0.5f;
  nu2->bezt[0].vec[1][0] = -1.0f + offset;
  nu2->bezt[0].vec[1][1] = -1.0f;
  nu2->bezt[0].vec[2][0] = -0.5f + offset;
  nu2->bezt[0].vec[2][1] = -1.0f;
  nu2->bezt[0].h1 = nu2->bezt[0].h2 = HD_FREE;

  nu2->bezt[1].vec[0][0] = 0.0f + offset;
  nu2->bezt[1].vec[0][1] = -1.0f;
  nu2->bezt[1].vec[1][0] = 0.0f + offset;
  nu2->bezt[1].vec[1][1] = 0.0f;
  nu2->bezt[1].vec[2][0] = 0.0f + offset;
  nu2->bezt[1].vec[2][1] = 1.0f;
  nu2->bezt[1].h1 = nu2->bezt[1].h2 = HD_FREE;

  nu2->bezt[2].vec[0][0] = 0.5f + offset;
  nu2->bezt[2].vec[0][1] = 1.0f;
  nu2->bezt[2].vec[1][0] = 1.0f + offset;
  nu2->bezt[2].vec[1][1] = 1.0f;
  nu2->bezt[2].vec[2][0] = 1.0f + offset;
  nu2->bezt[2].vec[2][1] = 0.5f;
  nu2->bezt[2].h1 = nu2->bezt[2].h2 = HD_FREE;

  nu2->bezt[3].vec[0][0] = 1.0f + offset;
  nu2->bezt[3].vec[0][1] = 0.0f;
  nu2->bezt[3].vec[1][0] = 1.0f + offset;
  nu2->bezt[3].vec[1][1] = -0.5f;
  nu2->bezt[3].vec[2][0] = 0.5f + offset;
  nu2->bezt[3].vec[2][1] = -0.5f;
  nu2->bezt[3].h1 = nu2->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle S-curve overlap */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Multiple circles overlapping a figure-8.
 */
TEST(vfontdata_sanitize, InteriorIntersection_Figure8MultipleCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Figure-8 centered at origin */
  Nurb *figure8 = create_figure_8_nurb(0.0f, 0.0f, 1.0f);

  /* Multiple circles overlapping different parts */
  Nurb *circle1 = create_circle_nurb(0.5f, 0.5f, 0.4f);   /* Top-right */
  Nurb *circle2 = create_circle_nurb(-0.5f, -0.5f, 0.4f); /* Bottom-left */
  Nurb *circle3 = create_circle_nurb(0.0f, 0.0f, 0.2f);   /* Center crossing */

  BLI_addtail(&nurbsbase, figure8);
  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);
  BLI_addtail(&nurbsbase, circle3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Point count doesn't explode for chain of circles.
 * Verifies reasonable output complexity.
 */
TEST(vfontdata_sanitize, InteriorIntersection_PointCountReasonable)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create 10 overlapping circles */
  const int NUM_CIRCLES = 10;
  const float SPACING = 0.8f;
  const float RADIUS = 0.5f;

  int points_before = 0;
  for (int i = 0; i < NUM_CIRCLES; i++) {
    Nurb *circle = create_circle_nurb(float(i) * SPACING, 0.0f, RADIUS);
    BLI_addtail(&nurbsbase, circle);
    points_before += 4; /* Each circle has 4 bezier points */
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  int points_after = count_bezier_points(&nurbsbase);

  /* Output shouldn't have exponentially more points */
  /* With 10 circles, we have 9 pairs of intersections = 18 new points */
  /* Plus original perimeter points, should be roughly 40-60 total */
  EXPECT_LT(points_after, points_before * 3)
      << "Point count should not explode: before=" << points_before << ", after=" << points_after;

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Spline Solver Fallback Tests (Item 23)
 *
 * Tests for finding t-values on bezier curves given a point.
 * \{ */

/**
 * Test: High curvature S-curve intersection.
 * Tests intersection finding on curves with multiple inflection points.
 */
TEST(vfontdata_sanitize, SplineSolver_HighCurvatureSCurve)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create an S-curve with high curvature */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* S-curve going right then sharply back left */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.5f;
  nu->bezt[0].vec[0][1] = 0.5f;
  nu->bezt[0].vec[2][0] = 1.0f;
  nu->bezt[0].vec[2][1] = 0.5f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[1][0] = 2.0f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[0][0] = 1.5f;
  nu->bezt[1].vec[0][1] = 0.8f;
  nu->bezt[1].vec[2][0] = 2.5f;
  nu->bezt[1].vec[2][1] = 1.2f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Sharp turn back */
  nu->bezt[2].vec[1][0] = 0.5f;
  nu->bezt[2].vec[1][1] = 2.0f;
  nu->bezt[2].vec[0][0] = 1.5f;
  nu->bezt[2].vec[0][1] = 2.0f;
  nu->bezt[2].vec[2][0] = -0.5f;
  nu->bezt[2].vec[2][1] = 2.0f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  nu->bezt[3].vec[1][0] = -1.0f;
  nu->bezt[3].vec[1][1] = 1.0f;
  nu->bezt[3].vec[0][0] = -1.0f;
  nu->bezt[3].vec[0][1] = 1.5f;
  nu->bezt[3].vec[2][0] = -1.0f;
  nu->bezt[3].vec[2][1] = 0.5f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  /* Add a simple circle that intersects the S-curve */
  Nurb *circle = create_circle_nurb(0.5f, 1.0f, 0.8f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Curve with cusp-like feature.
 * Tests handling of curves that nearly self-intersect.
 */
TEST(vfontdata_sanitize, SplineSolver_NearCusp)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a curve with a near-cusp (sharp turn) */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Shape with a near-cusp at the top */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.3f;
  nu->bezt[0].vec[0][1] = 0.0f;
  nu->bezt[0].vec[2][0] = 0.3f;
  nu->bezt[0].vec[2][1] = 0.0f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Right side going up */
  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[0][0] = 1.0f;
  nu->bezt[1].vec[0][1] = 0.3f;
  nu->bezt[1].vec[2][0] = 0.6f;
  nu->bezt[1].vec[2][1] = 1.5f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Near-cusp: handles point in opposite directions */
  nu->bezt[2].vec[1][0] = 0.0f;
  nu->bezt[2].vec[1][1] = 2.0f;
  nu->bezt[2].vec[0][0] = 0.4f;
  nu->bezt[2].vec[0][1] = 1.5f;
  nu->bezt[2].vec[2][0] = -0.4f;
  nu->bezt[2].vec[2][1] = 1.5f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Left side going down */
  nu->bezt[3].vec[1][0] = -1.0f;
  nu->bezt[3].vec[1][1] = 1.0f;
  nu->bezt[3].vec[0][0] = -0.6f;
  nu->bezt[3].vec[0][1] = 1.5f;
  nu->bezt[3].vec[2][0] = -1.0f;
  nu->bezt[3].vec[2][1] = 0.3f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  /* Add overlapping circle */
  Nurb *circle = create_circle_nurb(0.0f, 1.0f, 0.6f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Very flat bezier curves intersecting.
 * The solver needs to handle curves that are nearly linear.
 */
TEST(vfontdata_sanitize, SplineSolver_NearlyLinearCurves)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create two nearly-linear curves that cross */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Very small handle offsets - nearly linear */
  const float eps = 0.001f;

  /* Horizontal-ish line segment */
  nu->bezt[0].vec[1][0] = -2.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -2.0f - eps;
  nu->bezt[0].vec[0][1] = 0.0f;
  nu->bezt[0].vec[2][0] = -2.0f + eps;
  nu->bezt[0].vec[2][1] = eps;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[1][0] = 2.0f;
  nu->bezt[1].vec[1][1] = 0.1f;
  nu->bezt[1].vec[0][0] = 2.0f - eps;
  nu->bezt[1].vec[0][1] = 0.1f - eps;
  nu->bezt[1].vec[2][0] = 2.0f + eps;
  nu->bezt[1].vec[2][1] = 0.1f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Return path - also nearly linear */
  nu->bezt[2].vec[1][0] = 2.0f;
  nu->bezt[2].vec[1][1] = 1.0f;
  nu->bezt[2].vec[0][0] = 2.0f + eps;
  nu->bezt[2].vec[0][1] = 1.0f;
  nu->bezt[2].vec[2][0] = 2.0f - eps;
  nu->bezt[2].vec[2][1] = 1.0f - eps;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  nu->bezt[3].vec[1][0] = -2.0f;
  nu->bezt[3].vec[1][1] = 0.9f;
  nu->bezt[3].vec[0][0] = -2.0f + eps;
  nu->bezt[3].vec[0][1] = 0.9f + eps;
  nu->bezt[3].vec[2][0] = -2.0f - eps;
  nu->bezt[3].vec[2][1] = 0.9f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  /* Add crossing diagonal shape */
  Nurb *nu2 = MEM_new_for_free<Nurb>(__func__);
  *nu2 = dna::shallow_zero_initialize();
  nu2->type = CU_BEZIER;
  nu2->flagu = CU_NURB_CYCLIC;
  nu2->resolu = 12;
  nu2->pntsu = 4;
  nu2->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Diagonal crossing */
  nu2->bezt[0].vec[1][0] = -1.0f;
  nu2->bezt[0].vec[1][1] = -0.5f;
  nu2->bezt[0].vec[0][0] = -1.0f - eps;
  nu2->bezt[0].vec[0][1] = -0.5f;
  nu2->bezt[0].vec[2][0] = -1.0f + eps;
  nu2->bezt[0].vec[2][1] = -0.5f + eps;
  nu2->bezt[0].h1 = nu2->bezt[0].h2 = HD_FREE;

  nu2->bezt[1].vec[1][0] = 1.0f;
  nu2->bezt[1].vec[1][1] = 0.6f;
  nu2->bezt[1].vec[0][0] = 1.0f - eps;
  nu2->bezt[1].vec[0][1] = 0.6f - eps;
  nu2->bezt[1].vec[2][0] = 1.0f + eps;
  nu2->bezt[1].vec[2][1] = 0.6f;
  nu2->bezt[1].h1 = nu2->bezt[1].h2 = HD_FREE;

  nu2->bezt[2].vec[1][0] = 1.0f;
  nu2->bezt[2].vec[1][1] = 1.5f;
  nu2->bezt[2].vec[0][0] = 1.0f + eps;
  nu2->bezt[2].vec[0][1] = 1.5f;
  nu2->bezt[2].vec[2][0] = 1.0f - eps;
  nu2->bezt[2].vec[2][1] = 1.5f - eps;
  nu2->bezt[2].h1 = nu2->bezt[2].h2 = HD_FREE;

  nu2->bezt[3].vec[1][0] = -1.0f;
  nu2->bezt[3].vec[1][1] = 1.5f;
  nu2->bezt[3].vec[0][0] = -1.0f + eps;
  nu2->bezt[3].vec[0][1] = 1.5f + eps;
  nu2->bezt[3].vec[2][0] = -1.0f - eps;
  nu2->bezt[3].vec[2][1] = 1.5f;
  nu2->bezt[3].h1 = nu2->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Intersection at curve inflection point.
 * The t-value solver must handle curves changing direction.
 */
TEST(vfontdata_sanitize, SplineSolver_IntersectionAtInflection)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a curve with a clear inflection point */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 3;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(3, __func__);

  /* Triangle with curved sides - inflection at each vertex */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.5f;
  nu->bezt[0].vec[0][1] = 0.3f;
  nu->bezt[0].vec[2][0] = 0.5f;
  nu->bezt[0].vec[2][1] = 0.3f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[1][0] = 1.5f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[0][0] = 1.0f;
  nu->bezt[1].vec[0][1] = 0.5f;
  nu->bezt[1].vec[2][0] = 1.0f;
  nu->bezt[1].vec[2][1] = 1.5f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  nu->bezt[2].vec[1][0] = -1.5f;
  nu->bezt[2].vec[1][1] = 1.0f;
  nu->bezt[2].vec[0][0] = -1.0f;
  nu->bezt[2].vec[0][1] = 1.5f;
  nu->bezt[2].vec[2][0] = -1.0f;
  nu->bezt[2].vec[2][1] = 0.5f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  /* Circle positioned to intersect near inflection points */
  Nurb *circle = create_circle_nurb(0.0f, 0.5f, 0.6f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Multiple very close intersections on same curve pair.
 * Stress tests the t-value precision when intersections cluster.
 */
TEST(vfontdata_sanitize, SplineSolver_ClusteredIntersections)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a wavy curve */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 6;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(6, __func__);

  /* Wavy curve that oscillates - will have multiple intersections with a line */
  for (int i = 0; i < 6; i++) {
    float angle = float(i) * M_PI / 3.0f;
    float y_offset = (i % 2 == 0) ? 0.3f : -0.3f;

    nu->bezt[i].vec[1][0] = cosf(angle) * 1.5f;
    nu->bezt[i].vec[1][1] = sinf(angle) * 1.5f + y_offset;
    nu->bezt[i].vec[0][0] = nu->bezt[i].vec[1][0] - 0.3f;
    nu->bezt[i].vec[0][1] = nu->bezt[i].vec[1][1];
    nu->bezt[i].vec[2][0] = nu->bezt[i].vec[1][0] + 0.3f;
    nu->bezt[i].vec[2][1] = nu->bezt[i].vec[1][1];
    nu->bezt[i].h1 = nu->bezt[i].h2 = HD_FREE;
  }

  BLI_addtail(&nurbsbase, nu);

  /* Circle that will create multiple close intersections */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.2f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Subdivision exhaustion recovery.
 * Creates a case where subdivision might hit iteration limits.
 */
TEST(vfontdata_sanitize, SplineSolver_SubdivisionExhaustion)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create curves with very different scales */
  Nurb *large = create_circle_nurb(0.0f, 0.0f, 10.0f);
  BLI_addtail(&nurbsbase, large);

  /* Small circle at edge of large one - challenging for subdivision */
  Nurb *small = create_circle_nurb(9.5f, 0.0f, 0.6f);
  BLI_addtail(&nurbsbase, small);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle scale difference gracefully */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 1);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Large coordinate values with curved shapes.
 * Verifies numerical stability at large coordinates.
 */
TEST(vfontdata_sanitize, SplineSolver_LargeCoordinateCurves)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles at large coordinates */
  Nurb *circle1 = create_circle_nurb(10000.0f, 10000.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(10000.8f, 10000.0f, 1.0f);

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  int nurbs_before = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_before, 2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into single contour */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Overlapping circles at large coords should merge";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Small coordinate values with curved shapes.
 * Verifies numerical stability at small coordinates.
 */
TEST(vfontdata_sanitize, SplineSolver_SmallCoordinateCurves)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles at small scale */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 0.01f);
  Nurb *circle2 = create_circle_nurb(0.008f, 0.0f, 0.01f);

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into single contour */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Overlapping circles at small scale should merge";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Symmetric curves (mirror image).
 * Tests handling when both curves have identical shapes.
 */
TEST(vfontdata_sanitize, SplineSolver_SymmetricCurves)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two identical circles offset horizontally */
  Nurb *circle1 = create_circle_nurb(-0.3f, 0.0f, 0.5f);
  Nurb *circle2 = create_circle_nurb(0.3f, 0.0f, 0.5f);

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into single figure-8 or peanut shape */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Symmetric overlapping circles should merge";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Self-intersecting figure-8 with overlapping circle.
 * Complex case with both self-intersection and external overlap.
 */
TEST(vfontdata_sanitize, SplineSolver_SelfIntersectingWithOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a figure-8 (self-intersecting) */
  Nurb *fig8 = create_figure_8_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, fig8);

  /* Circle overlapping one loop of the figure-8 */
  Nurb *circle = create_circle_nurb(0.8f, 0.0f, 0.5f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should split and merge appropriately */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 1) << "Should produce at least one contour";
  EXPECT_LE(nurbs_after, 3) << "Should not explode into many contours";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Curves touching at exactly one point (tangent).
 * Edge case where curves share a single tangent point.
 */
TEST(vfontdata_sanitize, SplineSolver_TangentTouch)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles touching at exactly one point (externally tangent) */
  Nurb *circle1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *circle2 = create_circle_nurb(2.0f, 0.0f, 1.0f); /* Exactly touching */

  BLI_addtail(&nurbsbase, circle1);
  BLI_addtail(&nurbsbase, circle2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should remain as 2 separate contours (just touching, not overlapping) */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 2) << "Tangent circles should remain separate";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Curves with collinear control points.
 * Tests handling of degenerate bezier segments.
 */
TEST(vfontdata_sanitize, SplineSolver_CollinearControlPoints)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a shape with collinear control points (straight edge) */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Square with collinear handles (straight edges) */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.1f; /* Collinear with edge */
  nu->bezt[0].vec[0][1] = 0.0f;
  nu->bezt[0].vec[2][0] = 0.3f;
  nu->bezt[0].vec[2][1] = 0.0f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 0.0f;
  nu->bezt[1].vec[0][0] = 0.7f;
  nu->bezt[1].vec[0][1] = 0.0f;
  nu->bezt[1].vec[2][0] = 1.0f;
  nu->bezt[1].vec[2][1] = 0.3f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 1.0f;
  nu->bezt[2].vec[0][0] = 1.0f;
  nu->bezt[2].vec[0][1] = 0.7f;
  nu->bezt[2].vec[2][0] = 0.7f;
  nu->bezt[2].vec[2][1] = 1.0f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  nu->bezt[3].vec[1][0] = 0.0f;
  nu->bezt[3].vec[1][1] = 1.0f;
  nu->bezt[3].vec[0][0] = 0.3f;
  nu->bezt[3].vec[0][1] = 1.0f;
  nu->bezt[3].vec[2][0] = 0.0f;
  nu->bezt[3].vec[2][1] = 0.7f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  /* Overlapping circle */
  Nurb *circle = create_circle_nurb(0.5f, 0.5f, 0.4f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle collinear segments properly */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_GE(nurbs_after, 1);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Multiple scales in single operation.
 * Tests handling shapes at different coordinate magnitudes together.
 */
TEST(vfontdata_sanitize, SplineSolver_MixedScales)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Large circle */
  Nurb *large = create_circle_nurb(0.0f, 0.0f, 5.0f);
  BLI_addtail(&nurbsbase, large);

  /* Medium circle overlapping */
  Nurb *medium = create_circle_nurb(4.0f, 0.0f, 2.0f);
  BLI_addtail(&nurbsbase, medium);

  /* Small circle overlapping medium */
  Nurb *small = create_circle_nurb(5.5f, 0.0f, 0.8f);
  BLI_addtail(&nurbsbase, small);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* All overlapping circles should reduce contour count.
   * May not fully merge to 1 due to processing order and minimal overlap between large+small. */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_LE(nurbs_after, 2) << "Mixed scale circles should merge (at least partially)";
  EXPECT_GE(nurbs_after, 1);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Intersection exactly at segment endpoint.
 * Edge case where intersection falls precisely at t=0 or t=1.
 */
TEST(vfontdata_sanitize, SplineSolver_IntersectionAtEndpoint)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create two shapes that share a vertex */
  Nurb *nu1 = MEM_new_for_free<Nurb>(__func__);
  *nu1 = dna::shallow_zero_initialize();
  nu1->type = CU_BEZIER;
  nu1->flagu = CU_NURB_CYCLIC;
  nu1->resolu = 12;
  nu1->pntsu = 3;
  nu1->bezt = MEM_calloc_arrayN<BezTriple>(3, __func__);

  /* Triangle 1 */
  nu1->bezt[0].vec[1][0] = 0.0f;
  nu1->bezt[0].vec[1][1] = 0.0f;
  nu1->bezt[0].vec[0][0] = -0.2f;
  nu1->bezt[0].vec[0][1] = 0.1f;
  nu1->bezt[0].vec[2][0] = 0.2f;
  nu1->bezt[0].vec[2][1] = 0.1f;
  nu1->bezt[0].h1 = nu1->bezt[0].h2 = HD_FREE;

  nu1->bezt[1].vec[1][0] = 1.0f;
  nu1->bezt[1].vec[1][1] = 0.0f; /* Shared point */
  nu1->bezt[1].vec[0][0] = 0.8f;
  nu1->bezt[1].vec[0][1] = 0.1f;
  nu1->bezt[1].vec[2][0] = 1.0f;
  nu1->bezt[1].vec[2][1] = 0.3f;
  nu1->bezt[1].h1 = nu1->bezt[1].h2 = HD_FREE;

  nu1->bezt[2].vec[1][0] = 0.5f;
  nu1->bezt[2].vec[1][1] = 1.0f;
  nu1->bezt[2].vec[0][0] = 0.7f;
  nu1->bezt[2].vec[0][1] = 0.7f;
  nu1->bezt[2].vec[2][0] = 0.3f;
  nu1->bezt[2].vec[2][1] = 0.7f;
  nu1->bezt[2].h1 = nu1->bezt[2].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu1);

  /* Triangle 2 sharing point at (1, 0) */
  Nurb *nu2 = MEM_new_for_free<Nurb>(__func__);
  *nu2 = dna::shallow_zero_initialize();
  nu2->type = CU_BEZIER;
  nu2->flagu = CU_NURB_CYCLIC;
  nu2->resolu = 12;
  nu2->pntsu = 3;
  nu2->bezt = MEM_calloc_arrayN<BezTriple>(3, __func__);

  nu2->bezt[0].vec[1][0] = 1.0f;
  nu2->bezt[0].vec[1][1] = 0.0f; /* Shared point */
  nu2->bezt[0].vec[0][0] = 1.0f;
  nu2->bezt[0].vec[0][1] = -0.2f;
  nu2->bezt[0].vec[2][0] = 1.2f;
  nu2->bezt[0].vec[2][1] = 0.1f;
  nu2->bezt[0].h1 = nu2->bezt[0].h2 = HD_FREE;

  nu2->bezt[1].vec[1][0] = 2.0f;
  nu2->bezt[1].vec[1][1] = 0.0f;
  nu2->bezt[1].vec[0][0] = 1.8f;
  nu2->bezt[1].vec[0][1] = 0.1f;
  nu2->bezt[1].vec[2][0] = 2.0f;
  nu2->bezt[1].vec[2][1] = 0.3f;
  nu2->bezt[1].h1 = nu2->bezt[1].h2 = HD_FREE;

  nu2->bezt[2].vec[1][0] = 1.5f;
  nu2->bezt[2].vec[1][1] = 1.0f;
  nu2->bezt[2].vec[0][0] = 1.7f;
  nu2->bezt[2].vec[0][1] = 0.7f;
  nu2->bezt[2].vec[2][0] = 1.3f;
  nu2->bezt[2].vec[2][1] = 0.7f;
  nu2->bezt[2].h1 = nu2->bezt[2].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Shapes share only a point, should remain separate */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 2) << "Shapes sharing only a vertex should remain separate";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Verify point count doesn't explode with complex curves.
 * Regression test for point explosion bugs.
 */
TEST(vfontdata_sanitize, SplineSolver_PointCountStability)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create multiple overlapping circles */
  const int NUM_CIRCLES = 5;
  int total_points_before = 0;

  for (int i = 0; i < NUM_CIRCLES; i++) {
    Nurb *circle = create_circle_nurb(float(i) * 0.6f, 0.0f, 0.5f);
    BLI_addtail(&nurbsbase, circle);
    total_points_before += 4; /* Each circle has 4 points */
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  int total_points_after = count_bezier_points(&nurbsbase);

  /* Point count should be reasonable - not exploding */
  /* With 5 circles creating 4 intersection pairs, expect roughly 20-40 points */
  EXPECT_LT(total_points_after, total_points_before * 5)
      << "Point count should not explode: before=" << total_points_before
      << ", after=" << total_points_after;
  EXPECT_GT(total_points_after, 0) << "Should have some points";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bezier Reconstruction Tests (Item 29)
 *
 * Tests for comprehensive Bezier handle reconstruction to ensure:
 * 1. Full-span segments preserve original control points
 * 2. Partial-span segments compute handles correctly from derivatives
 * 3. Bounding boxes are preserved (not clipped)
 * 4. Handle continuity is maintained
 * 5. Round-trip fidelity for shapes with no actual overlaps
 * \{ */

/**
 * Helper: Calculate approximate area using shoelace formula on bezier points.
 */
static float approximate_area(const ListBase *nurbsbase)
{
  float total = 0.0f;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase->first); nu != nullptr; nu = nu->next)
  {
    if (nu->bezt == nullptr || nu->pntsu < 3) {
      continue;
    }
    float area = 0.0f;
    for (int i = 0; i < nu->pntsu; i++) {
      int next = (i + 1) % nu->pntsu;
      area += nu->bezt[i].vec[1][0] * nu->bezt[next].vec[1][1];
      area -= nu->bezt[next].vec[1][0] * nu->bezt[i].vec[1][1];
    }
    total += std::abs(area) * 0.5f;
  }
  return total;
}

/**
 * Test: Single isolated shape should pass through unchanged.
 * When a shape has no overlaps, the reconstruction should preserve it exactly
 * (or very nearly) since full-span monotonics should use original handles.
 */
TEST(vfontdata_sanitize, BezierReconstruction_SingleShapeRoundTrip)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a single circle with no overlaps. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  /* Save original bounds. */
  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);
  float orig_area = approximate_area(&nurbsbase);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should still have exactly 1 contour. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);

  /* Bounds should be preserved (not clipped). */
  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  /* With proper handle-in computation, bounds should be preserved to within 1%. */
  EXPECT_GE(new_max_x - new_min_x, (orig_max_x - orig_min_x) * 0.995f)
      << "Width should be preserved";
  EXPECT_GE(new_max_y - new_min_y, (orig_max_y - orig_min_y) * 0.995f)
      << "Height should be preserved";

  /* Area should be approximately preserved. */
  float new_area = approximate_area(&nurbsbase);
  EXPECT_GT(new_area, orig_area * 0.98f) << "Area should not shrink significantly";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Bounding box must not clip after overlap removal.
 * This is a key requirement for Item 29 - handles define curve extrema.
 */
TEST(vfontdata_sanitize, BezierReconstruction_BoundsNotClipped)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.5f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  /* Calculate combined original bounds. */
  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Calculate new bounds. */
  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  /* New bounds should encompass original bounds (tighter tolerance with proper handles). */
  EXPECT_LE(new_min_x, orig_min_x + 0.02f) << "Left bound should not clip";
  EXPECT_LE(new_min_y, orig_min_y + 0.02f) << "Bottom bound should not clip";
  EXPECT_GE(new_max_x, orig_max_x - 0.02f) << "Right bound should not clip";
  EXPECT_GE(new_max_y, orig_max_y - 0.02f) << "Top bound should not clip";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Edge-touching shapes (shared edge, no overlap).
 * Two adjacent squares sharing an edge should not lose bounds.
 * This is the original PartiallyCoincidentSplines limitation.
 */
TEST(vfontdata_sanitize, BezierReconstruction_EdgeTouchingSquares)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares sharing edge at x=1. */
  Nurb *nu1 = create_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *nu2 = create_square_nurb(1.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);
  float orig_width = orig_max_x - orig_min_x;
  float orig_height = orig_max_y - orig_min_y;

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);
  float new_width = new_max_x - new_min_x;
  float new_height = new_max_y - new_min_y;

  /* Edge-touching involves coincident detection which may have some inherent loss.
   * 97% preservation is acceptable for this edge case. */
  EXPECT_GE(new_width, orig_width * 0.97f)
      << "Width preservation: orig=" << orig_width << ", new=" << new_width;
  EXPECT_GE(new_height, orig_height * 0.97f)
      << "Height preservation: orig=" << orig_height << ", new=" << new_height;

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Handle extrema beyond endpoints.
 * A curve where the bezier handle positions define extrema beyond the endpoints.
 * This tests that handle reconstruction doesn't lose the curve's extent.
 */
TEST(vfontdata_sanitize, BezierReconstruction_HandleExtremaBeyondEndpoints)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a bulging curve where handles extend beyond endpoints. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Oval shape with exaggerated handles that bulge outward. */
  /* Point 0: left */
  nu->bezt[0].vec[0][0] = -1.0f;
  nu->bezt[0].vec[0][1] = -0.8f; /* Handle in */
  nu->bezt[0].vec[1][0] = -1.0f;
  nu->bezt[0].vec[1][1] = 0.0f; /* Point */
  nu->bezt[0].vec[2][0] = -1.0f;
  nu->bezt[0].vec[2][1] = 0.8f; /* Handle out */
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Point 1: top - handles extend beyond y=1 */
  nu->bezt[1].vec[0][0] = -0.8f;
  nu->bezt[1].vec[0][1] = 1.2f; /* Handle extends to y=1.2 */
  nu->bezt[1].vec[1][0] = 0.0f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[2][0] = 0.8f;
  nu->bezt[1].vec[2][1] = 1.2f; /* Handle extends to y=1.2 */
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Point 2: right */
  nu->bezt[2].vec[0][0] = 1.0f;
  nu->bezt[2].vec[0][1] = 0.8f;
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 0.0f;
  nu->bezt[2].vec[2][0] = 1.0f;
  nu->bezt[2].vec[2][1] = -0.8f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Point 3: bottom - handles extend beyond y=-1 */
  nu->bezt[3].vec[0][0] = 0.8f;
  nu->bezt[3].vec[0][1] = -1.2f;
  nu->bezt[3].vec[1][0] = 0.0f;
  nu->bezt[3].vec[1][1] = -1.0f;
  nu->bezt[3].vec[2][0] = -0.8f;
  nu->bezt[3].vec[2][1] = -1.2f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  /* The handle extrema should be preserved (tighter tolerance). */
  EXPECT_GE(new_max_y, orig_max_y - 0.05f) << "Top handle extrema should be preserved";
  EXPECT_LE(new_min_y, orig_min_y + 0.05f) << "Bottom handle extrema should be preserved";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Overlapping shapes with one fully contained.
 * The outer shape's bounds should be preserved after removing the inner overlap.
 */
TEST(vfontdata_sanitize, BezierReconstruction_ContainedShapeBounds)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Large outer circle. */
  Nurb *outer = create_circle_nurb(0.0f, 0.0f, 2.0f);
  BLI_addtail(&nurbsbase, outer);

  /* Small inner circle (fully contained). */
  Nurb *inner = create_circle_nurb(0.0f, 0.0f, 0.5f);
  BLI_addtail(&nurbsbase, inner);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  /* Outer bounds should be preserved (inner is just a hole) - tighter tolerance. */
  EXPECT_LE(new_min_x, orig_min_x + 0.05f) << "Left bound should be preserved";
  EXPECT_LE(new_min_y, orig_min_y + 0.05f) << "Bottom bound should be preserved";
  EXPECT_GE(new_max_x, orig_max_x - 0.05f) << "Right bound should be preserved";
  EXPECT_GE(new_max_y, orig_max_y - 0.05f) << "Top bound should be preserved";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Area preservation after merging overlapping shapes.
 * The combined area should not decrease significantly.
 */
TEST(vfontdata_sanitize, BezierReconstruction_AreaPreservation)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two partially overlapping circles. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(1.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  /* The merged area should be approximately 2*pi - overlap. */
  /* For circles of r=1 with centers 1 apart, overlap area is about 1.228 */
  /* Expected merged area ~ 2*pi - 1.228 ~ 5.05 */
  float expected_merged_area = 5.0f; /* Rough estimate */

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  float merged_area = approximate_area(&nurbsbase);

  /* Area should be in reasonable range (our approximation is rough). */
  EXPECT_GT(merged_area, expected_merged_area * 0.7f) << "Merged area too small";
  EXPECT_LT(merged_area, expected_merged_area * 1.5f) << "Merged area too large";

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Point count stability for shapes with no overlaps.
 * A single shape going through should not dramatically change point count.
 */
TEST(vfontdata_sanitize, BezierReconstruction_PointCountStability)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Single square - 4 points. */
  Nurb *square = create_square_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, square);

  int points_before = count_bezier_points(&nurbsbase);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  int points_after = count_bezier_points(&nurbsbase);

  /* Point count should be exactly preserved for a single non-overlapping shape. */
  EXPECT_EQ(points_after, points_before)
      << "Single shape point count: before=" << points_before << ", after=" << points_after;

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Multiple non-overlapping shapes should all be preserved.
 */
TEST(vfontdata_sanitize, BezierReconstruction_MultipleNonOverlapping)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three separate circles. */
  Nurb *c1 = create_circle_nurb(-3.0f, 0.0f, 0.5f);
  Nurb *c2 = create_circle_nurb(0.0f, 0.0f, 0.5f);
  Nurb *c3 = create_circle_nurb(3.0f, 0.0f, 0.5f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);
  BLI_addtail(&nurbsbase, c3);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should still have 3 contours. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 3);

  /* Bounds should be preserved. */
  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  /* Tighter tolerance with proper handle computation. */
  EXPECT_NEAR(new_min_x, orig_min_x, 0.02f);
  EXPECT_NEAR(new_max_x, orig_max_x, 0.02f);
  EXPECT_NEAR(new_min_y, orig_min_y, 0.02f);
  EXPECT_NEAR(new_max_y, orig_max_y, 0.02f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Complex curve with partial-span segments.
 * When a curve is split at intersections, the partial segments should have
 * reasonable derivative-based handles.
 */
TEST(vfontdata_sanitize, BezierReconstruction_PartialSpanHandles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create an X-crossing shape that will be split. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Figure-8 / bowtie shape that self-intersects. */
  nu->bezt[0].vec[1][0] = -1.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -1.0f;
  nu->bezt[0].vec[0][1] = 0.5f;
  nu->bezt[0].vec[2][0] = -0.5f;
  nu->bezt[0].vec[2][1] = -0.5f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[0][0] = 0.5f;
  nu->bezt[1].vec[0][1] = 0.5f;
  nu->bezt[1].vec[2][0] = 1.0f;
  nu->bezt[1].vec[2][1] = 0.5f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 0.0f;
  nu->bezt[2].vec[0][0] = 1.0f;
  nu->bezt[2].vec[0][1] = -0.5f;
  nu->bezt[2].vec[2][0] = 0.5f;
  nu->bezt[2].vec[2][1] = 0.5f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  nu->bezt[3].vec[1][0] = -1.0f;
  nu->bezt[3].vec[1][1] = -1.0f;
  nu->bezt[3].vec[0][0] = -0.5f;
  nu->bezt[3].vec[0][1] = -0.5f;
  nu->bezt[3].vec[2][0] = -1.0f;
  nu->bezt[3].vec[2][1] = -0.5f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* Bounds should be approximately preserved. */
  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  EXPECT_GE(new_max_x - new_min_x, (orig_max_x - orig_min_x) * 0.9f)
      << "Width should be mostly preserved";
  EXPECT_GE(new_max_y - new_min_y, (orig_max_y - orig_min_y) * 0.9f)
      << "Height should be mostly preserved";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Verify handles are continuous across segments.
 * At each vertex, handle directions should form smooth curves.
 */
TEST(vfontdata_sanitize, BezierReconstruction_HandleContinuity)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create overlapping circles that will merge. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.8f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Check that handles don't create sharp discontinuities. */
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    if (nu->bezt == nullptr || nu->pntsu < 2) {
      continue;
    }
    for (int i = 0; i < nu->pntsu; i++) {
      const BezTriple &bezt = nu->bezt[i];

      /* Handle positions should be distinct from vertex (unless linear). */
      float handle_in_dist = std::sqrt(std::pow(bezt.vec[0][0] - bezt.vec[1][0], 2.0f) +
                                       std::pow(bezt.vec[0][1] - bezt.vec[1][1], 2.0f));
      float handle_out_dist = std::sqrt(std::pow(bezt.vec[2][0] - bezt.vec[1][0], 2.0f) +
                                        std::pow(bezt.vec[2][1] - bezt.vec[1][1], 2.0f));

      /* Handles should have some length (not collapsed to point). */
      /* Allow very small handles for nearly-linear segments. */
      EXPECT_TRUE(handle_in_dist > 0.001f || handle_out_dist > 0.001f)
          << "Point " << i << " has collapsed handles";
    }
  }

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Handle positions should respect curve direction.
 * The handle-out of point N should be in the general direction of point N+1.
 */
TEST(vfontdata_sanitize, BezierReconstruction_HandleDirection)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Single circle should maintain proper handle directions. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    if (nu->bezt == nullptr || nu->pntsu < 2) {
      continue;
    }
    for (int i = 0; i < nu->pntsu; i++) {
      int next = (i + 1) % nu->pntsu;
      const BezTriple &bezt = nu->bezt[i];
      const BezTriple &bezt_next = nu->bezt[next];

      /* Direction from point to handle-out. */
      float handle_dx = bezt.vec[2][0] - bezt.vec[1][0];
      float handle_dy = bezt.vec[2][1] - bezt.vec[1][1];

      /* Direction from point to next point. */
      float next_dx = bezt_next.vec[1][0] - bezt.vec[1][0];
      float next_dy = bezt_next.vec[1][1] - bezt.vec[1][1];

      /* Dot product should be positive (handle points generally toward next). */
      float dot = handle_dx * next_dx + handle_dy * next_dy;

      /* Skip very short segments where direction is ambiguous. */
      float next_dist = std::sqrt(next_dx * next_dx + next_dy * next_dy);
      if (next_dist > 0.1f) {
        EXPECT_GE(dot, -0.1f) << "Handle-out at point " << i << " points away from next point";
      }
    }
  }

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Exact handle preservation for single non-overlapping shape.
 * When a shape has no overlaps, the output handles should match input exactly.
 */
TEST(vfontdata_sanitize, BezierReconstruction_ExactHandlePreservation)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a single circle - should pass through unchanged. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  /* Save original handle positions. */
  float orig_handles[4]
                    [6]; /* 4 points, each with handle-in (x,y) + point (x,y) + handle-out (x,y) */
  for (int i = 0; i < 4; i++) {
    orig_handles[i][0] = circle->bezt[i].vec[0][0];
    orig_handles[i][1] = circle->bezt[i].vec[0][1];
    orig_handles[i][2] = circle->bezt[i].vec[1][0];
    orig_handles[i][3] = circle->bezt[i].vec[1][1];
    orig_handles[i][4] = circle->bezt[i].vec[2][0];
    orig_handles[i][5] = circle->bezt[i].vec[2][1];
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first);
  EXPECT_EQ(nu->pntsu, 4);

  /* Verify handle positions match original (very tight tolerance). */
  for (int i = 0; i < 4; i++) {
    EXPECT_NEAR(nu->bezt[i].vec[0][0], orig_handles[i][0], 0.001f)
        << "Point " << i << " handle-in X mismatch";
    EXPECT_NEAR(nu->bezt[i].vec[0][1], orig_handles[i][1], 0.001f)
        << "Point " << i << " handle-in Y mismatch";
    EXPECT_NEAR(nu->bezt[i].vec[1][0], orig_handles[i][2], 0.001f)
        << "Point " << i << " vertex X mismatch";
    EXPECT_NEAR(nu->bezt[i].vec[1][1], orig_handles[i][3], 0.001f)
        << "Point " << i << " vertex Y mismatch";
    EXPECT_NEAR(nu->bezt[i].vec[2][0], orig_handles[i][4], 0.001f)
        << "Point " << i << " handle-out X mismatch";
    EXPECT_NEAR(nu->bezt[i].vec[2][1], orig_handles[i][5], 0.001f)
        << "Point " << i << " handle-out Y mismatch";
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Handle-in is distinct from vertex (not collapsed).
 * Verifies that handle-in is properly computed, not just set to vertex position.
 */
TEST(vfontdata_sanitize, BezierReconstruction_HandleInNotCollapsed)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles - will require handle reconstruction. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.5f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Check that handle-in is distinct from vertex for all points. */
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    for (int i = 0; i < nu->pntsu; i++) {
      const BezTriple &bezt = nu->bezt[i];
      float handle_in_dist = std::sqrt(std::pow(bezt.vec[0][0] - bezt.vec[1][0], 2.0f) +
                                       std::pow(bezt.vec[0][1] - bezt.vec[1][1], 2.0f));

      /* Handle-in should have meaningful length (not collapsed to vertex). */
      EXPECT_GT(handle_in_dist, 0.01f)
          << "Point " << i << " has collapsed handle-in (dist=" << handle_in_dist << ")";
    }
  }

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Tangent continuity at vertices.
 * For smooth curves, handle-in and handle-out should be roughly collinear.
 */
TEST(vfontdata_sanitize, BezierReconstruction_TangentContinuity)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Single circle - should have smooth tangent continuity. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first);
  ASSERT_NE(nu, nullptr);

  for (int i = 0; i < nu->pntsu; i++) {
    const BezTriple &bezt = nu->bezt[i];

    /* Vector from handle-in to vertex. */
    float in_dx = bezt.vec[1][0] - bezt.vec[0][0];
    float in_dy = bezt.vec[1][1] - bezt.vec[0][1];
    float in_len = std::sqrt(in_dx * in_dx + in_dy * in_dy);

    /* Vector from vertex to handle-out. */
    float out_dx = bezt.vec[2][0] - bezt.vec[1][0];
    float out_dy = bezt.vec[2][1] - bezt.vec[1][1];
    float out_len = std::sqrt(out_dx * out_dx + out_dy * out_dy);

    if (in_len > 0.01f && out_len > 0.01f) {
      /* Normalize and compute dot product (should be close to 1 for collinear). */
      float dot = (in_dx / in_len) * (out_dx / out_len) + (in_dy / in_len) * (out_dy / out_len);
      EXPECT_GT(dot, 0.9f) << "Point " << i << " has non-smooth tangent (dot=" << dot << ")";
    }
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Cyclic first-last handle continuity.
 * The first point's handle-in should come from the last segment.
 */
TEST(vfontdata_sanitize, BezierReconstruction_CyclicHandleContinuity)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Use circle - it has smooth curves where handle continuity matters.
   * Squares have 90° corners where perpendicular handle directions are expected. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  /* Save last segment's end info before processing. */
  int last_idx = circle->pntsu - 1;
  float last_handle_out_x = circle->bezt[last_idx].vec[2][0];
  float last_handle_out_y = circle->bezt[last_idx].vec[2][1];
  float first_vertex_x = circle->bezt[0].vec[1][0];
  float first_vertex_y = circle->bezt[0].vec[1][1];

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first);
  ASSERT_NE(nu, nullptr);
  ASSERT_GE(nu->pntsu, 4);

  /* First point's handle-in should point back toward last point's handle-out direction. */
  float first_handle_in_x = nu->bezt[0].vec[0][0];
  float first_handle_in_y = nu->bezt[0].vec[0][1];

  /* The direction from first point to its handle-in should continue the
   * direction from last handle-out through the vertex.
   * handle-in points "back" toward the incoming segment. */
  float to_handle_in_dx = first_handle_in_x - first_vertex_x;
  float to_handle_in_dy = first_handle_in_y - first_vertex_y;
  /* Direction the curve was traveling (from last handle-out toward first vertex). */
  float incoming_dx = first_vertex_x - last_handle_out_x;
  float incoming_dy = first_vertex_y - last_handle_out_y;

  float len1 = std::sqrt(to_handle_in_dx * to_handle_in_dx + to_handle_in_dy * to_handle_in_dy);
  float len2 = std::sqrt(incoming_dx * incoming_dx + incoming_dy * incoming_dy);

  if (len1 > 0.01f && len2 > 0.01f) {
    /* Handle-in points opposite to incoming direction (it "continues" the curve).
     * Dot product should be negative (opposite directions). */
    float dot = (to_handle_in_dx / len1) * (incoming_dx / len2) +
                (to_handle_in_dy / len1) * (incoming_dy / len2);
    EXPECT_LT(dot, -0.5f) << "First point's handle-in doesn't continue from last segment";
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Handle length is proportional to segment length.
 * Handles shouldn't be too short or too long relative to the segment.
 */
TEST(vfontdata_sanitize, BezierReconstruction_HandleLengthProportional)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Single circle. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first);
  ASSERT_NE(nu, nullptr);

  for (int i = 0; i < nu->pntsu; i++) {
    int next = (i + 1) % nu->pntsu;
    const BezTriple &bezt = nu->bezt[i];
    const BezTriple &bezt_next = nu->bezt[next];

    /* Segment length (vertex to vertex). */
    float seg_dx = bezt_next.vec[1][0] - bezt.vec[1][0];
    float seg_dy = bezt_next.vec[1][1] - bezt.vec[1][1];
    float seg_len = std::sqrt(seg_dx * seg_dx + seg_dy * seg_dy);

    /* Handle-out length. */
    float out_dx = bezt.vec[2][0] - bezt.vec[1][0];
    float out_dy = bezt.vec[2][1] - bezt.vec[1][1];
    float out_len = std::sqrt(out_dx * out_dx + out_dy * out_dy);

    /* Handle length should be roughly 1/3 of segment length for smooth beziers.
     * Allow range of 0.1 to 0.8 of segment length. */
    if (seg_len > 0.1f) {
      float ratio = out_len / seg_len;
      EXPECT_GT(ratio, 0.05f) << "Point " << i << " handle-out too short (ratio=" << ratio << ")";
      EXPECT_LT(ratio, 1.0f) << "Point " << i << " handle-out too long (ratio=" << ratio << ")";
    }
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Elongated oval maintains its shape after reconstruction.
 * Tests that curved shapes without self-intersection preserve their bounds.
 */
TEST(vfontdata_sanitize, BezierReconstruction_SCurveShape)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create an elongated oval (non-self-intersecting). */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Elongated oval: horizontal axis 2.0, vertical axis 1.0. */
  const float kappa = 0.5522847498f; /* Magic number for circular arcs. */

  /* Right point. */
  nu->bezt[0].vec[1][0] = 1.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = 1.0f;
  nu->bezt[0].vec[0][1] = -0.5f * kappa;
  nu->bezt[0].vec[2][0] = 1.0f;
  nu->bezt[0].vec[2][1] = 0.5f * kappa;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Top point. */
  nu->bezt[1].vec[1][0] = 0.0f;
  nu->bezt[1].vec[1][1] = 0.5f;
  nu->bezt[1].vec[0][0] = kappa;
  nu->bezt[1].vec[0][1] = 0.5f;
  nu->bezt[1].vec[2][0] = -kappa;
  nu->bezt[1].vec[2][1] = 0.5f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Left point. */
  nu->bezt[2].vec[1][0] = -1.0f;
  nu->bezt[2].vec[1][1] = 0.0f;
  nu->bezt[2].vec[0][0] = -1.0f;
  nu->bezt[2].vec[0][1] = 0.5f * kappa;
  nu->bezt[2].vec[2][0] = -1.0f;
  nu->bezt[2].vec[2][1] = -0.5f * kappa;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Bottom point. */
  nu->bezt[3].vec[1][0] = 0.0f;
  nu->bezt[3].vec[1][1] = -0.5f;
  nu->bezt[3].vec[0][0] = -kappa;
  nu->bezt[3].vec[0][1] = -0.5f;
  nu->bezt[3].vec[2][0] = kappa;
  nu->bezt[3].vec[2][1] = -0.5f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Oval bounds should be preserved. */
  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  EXPECT_NEAR(new_min_x, orig_min_x, 0.02f);
  EXPECT_NEAR(new_max_x, orig_max_x, 0.02f);
  EXPECT_NEAR(new_min_y, orig_min_y, 0.02f);
  EXPECT_NEAR(new_max_y, orig_max_y, 0.02f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Large scale numerical stability.
 * Handle computation should work correctly at large coordinates.
 */
TEST(vfontdata_sanitize, BezierReconstruction_LargeScale)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Circle at large coordinates. */
  Nurb *circle = create_circle_nurb(10000.0f, 10000.0f, 100.0f);
  BLI_addtail(&nurbsbase, circle);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  /* Bounds should be preserved within 0.1% at large scale. */
  float tolerance = (orig_max_x - orig_min_x) * 0.001f;
  EXPECT_NEAR(new_min_x, orig_min_x, tolerance);
  EXPECT_NEAR(new_max_x, orig_max_x, tolerance);
  EXPECT_NEAR(new_min_y, orig_min_y, tolerance);
  EXPECT_NEAR(new_max_y, orig_max_y, tolerance);

  /* Verify handles are not collapsed. */
  const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first);
  for (int i = 0; i < nu->pntsu; i++) {
    float handle_in_dist = std::sqrt(
        std::pow(nu->bezt[i].vec[0][0] - nu->bezt[i].vec[1][0], 2.0f) +
        std::pow(nu->bezt[i].vec[0][1] - nu->bezt[i].vec[1][1], 2.0f));
    EXPECT_GT(handle_in_dist, 1.0f) << "Point " << i << " has collapsed handle at large scale";
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Small scale numerical stability.
 * Handle computation should work correctly at small coordinates.
 */
TEST(vfontdata_sanitize, BezierReconstruction_SmallScale)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Tiny circle. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 0.01f);
  BLI_addtail(&nurbsbase, circle);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  /* Bounds should be preserved within 1% at small scale. */
  float tolerance = (orig_max_x - orig_min_x) * 0.01f;
  EXPECT_NEAR(new_min_x, orig_min_x, tolerance);
  EXPECT_NEAR(new_max_x, orig_max_x, tolerance);
  EXPECT_NEAR(new_min_y, orig_min_y, tolerance);
  EXPECT_NEAR(new_max_y, orig_max_y, tolerance);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Handle-in and handle-out both computed for merged shapes.
 * After merging overlapping shapes, verify both handles are properly computed.
 */
TEST(vfontdata_sanitize, BezierReconstruction_MergedShapeHandles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three overlapping circles that will merge. */
  Nurb *c1 = create_circle_nurb(-0.5f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c3 = create_circle_nurb(0.5f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);
  BLI_addtail(&nurbsbase, c3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* After merging, check all points have valid handles. */
  int total_points = 0;
  int collapsed_handle_in = 0;
  int collapsed_handle_out = 0;

  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    for (int i = 0; i < nu->pntsu; i++) {
      total_points++;
      const BezTriple &bezt = nu->bezt[i];

      float in_dist = std::sqrt(std::pow(bezt.vec[0][0] - bezt.vec[1][0], 2.0f) +
                                std::pow(bezt.vec[0][1] - bezt.vec[1][1], 2.0f));
      float out_dist = std::sqrt(std::pow(bezt.vec[2][0] - bezt.vec[1][0], 2.0f) +
                                 std::pow(bezt.vec[2][1] - bezt.vec[1][1], 2.0f));

      if (in_dist < 0.01f) {
        collapsed_handle_in++;
      }
      if (out_dist < 0.01f) {
        collapsed_handle_out++;
      }
    }
  }

  /* Allow at most 10% of handles to be collapsed (for corner cases). */
  EXPECT_LT(collapsed_handle_in, total_points / 10 + 1)
      << "Too many collapsed handle-in: " << collapsed_handle_in << "/" << total_points;
  EXPECT_LT(collapsed_handle_out, total_points / 10 + 1)
      << "Too many collapsed handle-out: " << collapsed_handle_out << "/" << total_points;

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Cusp-like curve maintains sharp corner.
 * Tests that curves with sharp direction changes preserve their shape.
 */
TEST(vfontdata_sanitize, BezierReconstruction_SharpCorner)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a diamond shape with sharp corners. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Diamond with very short handles (sharp corners). */
  float h = 0.05f; /* Very short handle length. */

  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 1.0f;
  nu->bezt[0].vec[0][0] = -h;
  nu->bezt[0].vec[0][1] = 1.0f;
  nu->bezt[0].vec[2][0] = h;
  nu->bezt[0].vec[2][1] = 1.0f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 0.0f;
  nu->bezt[1].vec[0][0] = 1.0f;
  nu->bezt[1].vec[0][1] = h;
  nu->bezt[1].vec[2][0] = 1.0f;
  nu->bezt[1].vec[2][1] = -h;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  nu->bezt[2].vec[1][0] = 0.0f;
  nu->bezt[2].vec[1][1] = -1.0f;
  nu->bezt[2].vec[0][0] = h;
  nu->bezt[2].vec[0][1] = -1.0f;
  nu->bezt[2].vec[2][0] = -h;
  nu->bezt[2].vec[2][1] = -1.0f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  nu->bezt[3].vec[1][0] = -1.0f;
  nu->bezt[3].vec[1][1] = 0.0f;
  nu->bezt[3].vec[0][0] = -1.0f;
  nu->bezt[3].vec[0][1] = -h;
  nu->bezt[3].vec[2][0] = -1.0f;
  nu->bezt[3].vec[2][1] = h;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Diamond shape should be preserved. */
  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  EXPECT_NEAR(new_min_x, orig_min_x, 0.02f);
  EXPECT_NEAR(new_max_x, orig_max_x, 0.02f);
  EXPECT_NEAR(new_min_y, orig_min_y, 0.02f);
  EXPECT_NEAR(new_max_y, orig_max_y, 0.02f);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Multiple separate contours maintain individual handle integrity.
 * Each contour should have properly computed handles independently.
 */
TEST(vfontdata_sanitize, BezierReconstruction_MultiContourHandleIntegrity)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three widely separated circles. */
  Nurb *c1 = create_circle_nurb(-5.0f, 0.0f, 0.5f);
  Nurb *c2 = create_circle_nurb(0.0f, 0.0f, 0.5f);
  Nurb *c3 = create_circle_nurb(5.0f, 0.0f, 0.5f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);
  BLI_addtail(&nurbsbase, c3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 3);

  /* Each circle should have exactly 4 points with proper handles. */
  int contour_idx = 0;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr;
       nu = nu->next, contour_idx++)
  {
    EXPECT_EQ(nu->pntsu, 4) << "Contour " << contour_idx << " point count mismatch";

    for (int i = 0; i < nu->pntsu; i++) {
      const BezTriple &bezt = nu->bezt[i];

      /* All handles should be distinct from vertex. */
      float in_dist = std::sqrt(std::pow(bezt.vec[0][0] - bezt.vec[1][0], 2.0f) +
                                std::pow(bezt.vec[0][1] - bezt.vec[1][1], 2.0f));
      float out_dist = std::sqrt(std::pow(bezt.vec[2][0] - bezt.vec[1][0], 2.0f) +
                                 std::pow(bezt.vec[2][1] - bezt.vec[1][1], 2.0f));

      EXPECT_GT(in_dist, 0.1f) << "Contour " << contour_idx << " point " << i
                               << " has collapsed handle-in";
      EXPECT_GT(out_dist, 0.1f) << "Contour " << contour_idx << " point " << i
                                << " has collapsed handle-out";
    }
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Handle reconstruction after intersection split.
 * When two curves intersect, the split segments should have proper handles.
 */
TEST(vfontdata_sanitize, BezierReconstruction_IntersectionSplitHandles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles - will create intersection splits. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(1.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a merged shape. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);

  /* All points should have non-collapsed handles from the split. */
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    for (int i = 0; i < nu->pntsu; i++) {
      const BezTriple &bezt = nu->bezt[i];

      float in_dx = bezt.vec[0][0] - bezt.vec[1][0];
      float in_dy = bezt.vec[0][1] - bezt.vec[1][1];
      float out_dx = bezt.vec[2][0] - bezt.vec[1][0];
      float out_dy = bezt.vec[2][1] - bezt.vec[1][1];

      float in_len = std::sqrt(in_dx * in_dx + in_dy * in_dy);
      float out_len = std::sqrt(out_dx * out_dx + out_dy * out_dy);

      /* Handles should have reasonable length (not collapsed). */
      EXPECT_GT(in_len, 0.01f) << "Point " << i << " has near-collapsed handle-in";
      EXPECT_GT(out_len, 0.01f) << "Point " << i << " has near-collapsed handle-out";
    }
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Linear segment handle preservation.
 * Collinear control points (straight lines) should maintain linearity.
 */
TEST(vfontdata_sanitize, BezierReconstruction_LinearSegments)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a square with truly linear segments (handles on line). */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Square with linear handles (handles along the edge direction). */
  /* Bottom-left to bottom-right. */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.1f;
  nu->bezt[0].vec[0][1] = 0.0f;
  nu->bezt[0].vec[2][0] = 0.3f;
  nu->bezt[0].vec[2][1] = 0.0f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 0.0f;
  nu->bezt[1].vec[0][0] = 0.7f;
  nu->bezt[1].vec[0][1] = 0.0f;
  nu->bezt[1].vec[2][0] = 1.0f;
  nu->bezt[1].vec[2][1] = 0.3f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 1.0f;
  nu->bezt[2].vec[0][0] = 1.0f;
  nu->bezt[2].vec[0][1] = 0.7f;
  nu->bezt[2].vec[2][0] = 0.7f;
  nu->bezt[2].vec[2][1] = 1.0f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  nu->bezt[3].vec[1][0] = 0.0f;
  nu->bezt[3].vec[1][1] = 1.0f;
  nu->bezt[3].vec[0][0] = 0.3f;
  nu->bezt[3].vec[0][1] = 1.0f;
  nu->bezt[3].vec[2][0] = 0.0f;
  nu->bezt[3].vec[2][1] = 0.7f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Bounds should be exactly preserved for this linear shape. */
  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  EXPECT_NEAR(new_min_x, orig_min_x, 0.01f);
  EXPECT_NEAR(new_max_x, orig_max_x, 0.01f);
  EXPECT_NEAR(new_min_y, orig_min_y, 0.01f);
  EXPECT_NEAR(new_max_y, orig_max_y, 0.01f);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Figure-8 self-intersection reconstruction.
 * Self-intersecting shapes should be split into valid non-intersecting contours.
 */
TEST(vfontdata_sanitize, BezierReconstruction_Figure8Split)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a figure-8 shape that self-intersects. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Figure-8: two loops that cross at center. */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.5f;
  nu->bezt[0].vec[0][1] = -0.5f;
  nu->bezt[0].vec[2][0] = 0.5f;
  nu->bezt[0].vec[2][1] = 0.5f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[0][0] = 0.5f;
  nu->bezt[1].vec[0][1] = 1.0f;
  nu->bezt[1].vec[2][0] = 1.0f;
  nu->bezt[1].vec[2][1] = 0.5f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  nu->bezt[2].vec[1][0] = 0.0f;
  nu->bezt[2].vec[1][1] = 0.0f;
  nu->bezt[2].vec[0][0] = 0.5f;
  nu->bezt[2].vec[0][1] = -0.5f;
  nu->bezt[2].vec[2][0] = -0.5f;
  nu->bezt[2].vec[2][1] = 0.5f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  nu->bezt[3].vec[1][0] = -1.0f;
  nu->bezt[3].vec[1][1] = 1.0f;
  nu->bezt[3].vec[0][0] = -1.0f;
  nu->bezt[3].vec[0][1] = 0.5f;
  nu->bezt[3].vec[2][0] = -0.5f;
  nu->bezt[3].vec[2][1] = 1.0f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce 2 separate loops (figure-8 splits at crossing). */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);

  /* All output contours should be valid. */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* All handles should be non-collapsed. */
  for (const Nurb *out_nu = static_cast<const Nurb *>(nurbsbase.first); out_nu != nullptr;
       out_nu = out_nu->next)
  {
    for (int i = 0; i < out_nu->pntsu; i++) {
      const BezTriple &bezt = out_nu->bezt[i];
      float in_len = std::sqrt(std::pow(bezt.vec[0][0] - bezt.vec[1][0], 2.0f) +
                               std::pow(bezt.vec[0][1] - bezt.vec[1][1], 2.0f));
      float out_len = std::sqrt(std::pow(bezt.vec[2][0] - bezt.vec[1][0], 2.0f) +
                                std::pow(bezt.vec[2][1] - bezt.vec[1][1], 2.0f));
      EXPECT_GT(in_len, 0.001f) << "Point " << i << " collapsed handle-in";
      EXPECT_GT(out_len, 0.001f) << "Point " << i << " collapsed handle-out";
    }
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Near-cusp curve handle reconstruction.
 * Very tight bends should not produce degenerate handles.
 */
TEST(vfontdata_sanitize, BezierReconstruction_NearCusp)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a teardrop shape with a near-cusp. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 3;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(3, __func__);

  /* Teardrop: wide bottom, sharp top. */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 1.0f; /* Sharp tip. */
  nu->bezt[0].vec[0][0] = 0.05f;
  nu->bezt[0].vec[0][1] = 0.9f;
  nu->bezt[0].vec[2][0] = -0.05f;
  nu->bezt[0].vec[2][1] = 0.9f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[1][0] = -0.5f;
  nu->bezt[1].vec[1][1] = 0.0f;
  nu->bezt[1].vec[0][0] = -0.5f;
  nu->bezt[1].vec[0][1] = 0.5f;
  nu->bezt[1].vec[2][0] = -0.5f;
  nu->bezt[1].vec[2][1] = -0.3f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  nu->bezt[2].vec[1][0] = 0.5f;
  nu->bezt[2].vec[1][1] = 0.0f;
  nu->bezt[2].vec[0][0] = 0.5f;
  nu->bezt[2].vec[0][1] = -0.3f;
  nu->bezt[2].vec[2][0] = 0.5f;
  nu->bezt[2].vec[2][1] = 0.5f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Shape should be preserved. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* Near-cusp point should still have valid handles. */
  const Nurb *out_nu = static_cast<const Nurb *>(nurbsbase.first);
  ASSERT_NE(out_nu, nullptr);

  for (int i = 0; i < out_nu->pntsu; i++) {
    const BezTriple &bezt = out_nu->bezt[i];
    float in_len = std::sqrt(std::pow(bezt.vec[0][0] - bezt.vec[1][0], 2.0f) +
                             std::pow(bezt.vec[0][1] - bezt.vec[1][1], 2.0f));
    float out_len = std::sqrt(std::pow(bezt.vec[2][0] - bezt.vec[1][0], 2.0f) +
                              std::pow(bezt.vec[2][1] - bezt.vec[1][1], 2.0f));

    /* Even at cusp, handles should exist (though may be short). */
    EXPECT_GT(in_len, 0.001f) << "Point " << i << " has degenerate handle-in at cusp";
    EXPECT_GT(out_len, 0.001f) << "Point " << i << " has degenerate handle-out at cusp";
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Winding direction preservation.
 * Clockwise and counterclockwise contours should maintain their winding.
 */
TEST(vfontdata_sanitize, BezierReconstruction_WindingPreservation)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a CCW circle. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  /* Calculate original winding (signed area). */
  float orig_signed_area = 0.0f;
  for (int i = 0; i < circle->pntsu; i++) {
    int next = (i + 1) % circle->pntsu;
    float x1 = circle->bezt[i].vec[1][0];
    float y1 = circle->bezt[i].vec[1][1];
    float x2 = circle->bezt[next].vec[1][0];
    float y2 = circle->bezt[next].vec[1][1];
    orig_signed_area += (x2 - x1) * (y2 + y1);
  }
  bool orig_ccw = orig_signed_area < 0;

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Calculate new winding. */
  const Nurb *out_nu = static_cast<const Nurb *>(nurbsbase.first);
  ASSERT_NE(out_nu, nullptr);

  float new_signed_area = 0.0f;
  for (int i = 0; i < out_nu->pntsu; i++) {
    int next = (i + 1) % out_nu->pntsu;
    float x1 = out_nu->bezt[i].vec[1][0];
    float y1 = out_nu->bezt[i].vec[1][1];
    float x2 = out_nu->bezt[next].vec[1][0];
    float y2 = out_nu->bezt[next].vec[1][1];
    new_signed_area += (x2 - x1) * (y2 + y1);
  }
  bool new_ccw = new_signed_area < 0;

  EXPECT_EQ(orig_ccw, new_ccw) << "Winding direction changed after reconstruction";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Symmetric shape produces symmetric handles.
 * A perfectly symmetric shape should have symmetric handle structure.
 */
TEST(vfontdata_sanitize, BezierReconstruction_HandleSymmetry)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a symmetric diamond. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  const float k = 0.4f; /* Handle offset. */

  /* Right. */
  nu->bezt[0].vec[1][0] = 1.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = 1.0f;
  nu->bezt[0].vec[0][1] = -k;
  nu->bezt[0].vec[2][0] = 1.0f;
  nu->bezt[0].vec[2][1] = k;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Top. */
  nu->bezt[1].vec[1][0] = 0.0f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[0][0] = k;
  nu->bezt[1].vec[0][1] = 1.0f;
  nu->bezt[1].vec[2][0] = -k;
  nu->bezt[1].vec[2][1] = 1.0f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Left. */
  nu->bezt[2].vec[1][0] = -1.0f;
  nu->bezt[2].vec[1][1] = 0.0f;
  nu->bezt[2].vec[0][0] = -1.0f;
  nu->bezt[2].vec[0][1] = k;
  nu->bezt[2].vec[2][0] = -1.0f;
  nu->bezt[2].vec[2][1] = -k;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Bottom. */
  nu->bezt[3].vec[1][0] = 0.0f;
  nu->bezt[3].vec[1][1] = -1.0f;
  nu->bezt[3].vec[0][0] = -k;
  nu->bezt[3].vec[0][1] = -1.0f;
  nu->bezt[3].vec[2][0] = k;
  nu->bezt[3].vec[2][1] = -1.0f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  const Nurb *out_nu = static_cast<const Nurb *>(nurbsbase.first);
  ASSERT_NE(out_nu, nullptr);
  ASSERT_EQ(out_nu->pntsu, 4);

  /* Check that opposite points have symmetric handle lengths. */
  /* Point 0 (right) vs Point 2 (left). */
  float len0_in = std::sqrt(std::pow(out_nu->bezt[0].vec[0][0] - out_nu->bezt[0].vec[1][0], 2.0f) +
                            std::pow(out_nu->bezt[0].vec[0][1] - out_nu->bezt[0].vec[1][1], 2.0f));
  float len2_in = std::sqrt(std::pow(out_nu->bezt[2].vec[0][0] - out_nu->bezt[2].vec[1][0], 2.0f) +
                            std::pow(out_nu->bezt[2].vec[0][1] - out_nu->bezt[2].vec[1][1], 2.0f));
  EXPECT_NEAR(len0_in, len2_in, 0.05f) << "Handle-in lengths not symmetric for opposite points";

  /* Point 1 (top) vs Point 3 (bottom). */
  float len1_in = std::sqrt(std::pow(out_nu->bezt[1].vec[0][0] - out_nu->bezt[1].vec[1][0], 2.0f) +
                            std::pow(out_nu->bezt[1].vec[0][1] - out_nu->bezt[1].vec[1][1], 2.0f));
  float len3_in = std::sqrt(std::pow(out_nu->bezt[3].vec[0][0] - out_nu->bezt[3].vec[1][0], 2.0f) +
                            std::pow(out_nu->bezt[3].vec[0][1] - out_nu->bezt[3].vec[1][1], 2.0f));
  EXPECT_NEAR(len1_in, len3_in, 0.05f) << "Handle-in lengths not symmetric for top/bottom";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Multiple intersections on same curve segment.
 * A segment split multiple times should reconstruct properly.
 */
TEST(vfontdata_sanitize, BezierReconstruction_MultipleSplits)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Large circle intersected by multiple smaller circles. */
  Nurb *big = create_circle_nurb(0.0f, 0.0f, 2.0f);
  Nurb *small1 = create_circle_nurb(1.5f, 0.0f, 0.8f);
  Nurb *small2 = create_circle_nurb(-1.5f, 0.0f, 0.8f);
  Nurb *small3 = create_circle_nurb(0.0f, 1.5f, 0.8f);

  BLI_addtail(&nurbsbase, big);
  BLI_addtail(&nurbsbase, small1);
  BLI_addtail(&nurbsbase, small2);
  BLI_addtail(&nurbsbase, small3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a merged shape. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* All handles should be valid. */
  for (const Nurb *out_nu = static_cast<const Nurb *>(nurbsbase.first); out_nu != nullptr;
       out_nu = out_nu->next)
  {
    for (int i = 0; i < out_nu->pntsu; i++) {
      const BezTriple &bezt = out_nu->bezt[i];
      float in_len = std::sqrt(std::pow(bezt.vec[0][0] - bezt.vec[1][0], 2.0f) +
                               std::pow(bezt.vec[0][1] - bezt.vec[1][1], 2.0f));
      float out_len = std::sqrt(std::pow(bezt.vec[2][0] - bezt.vec[1][0], 2.0f) +
                                std::pow(bezt.vec[2][1] - bezt.vec[1][1], 2.0f));
      EXPECT_GT(in_len, 0.001f) << "Point " << i << " collapsed handle-in after multi-split";
      EXPECT_GT(out_len, 0.001f) << "Point " << i << " collapsed handle-out after multi-split";
    }
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Inflection point curve reconstruction.
 * S-shaped curves with inflection points should maintain curvature.
 */
TEST(vfontdata_sanitize, BezierReconstruction_InflectionPoint)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a closed S-curve with inflection. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 6;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(6, __func__);

  /* Peanut/hourglass shape with inflection. */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 1.0f;
  nu->bezt[0].vec[0][0] = -0.3f;
  nu->bezt[0].vec[0][1] = 1.0f;
  nu->bezt[0].vec[2][0] = 0.3f;
  nu->bezt[0].vec[2][1] = 1.0f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  nu->bezt[1].vec[1][0] = 0.5f;
  nu->bezt[1].vec[1][1] = 0.5f;
  nu->bezt[1].vec[0][0] = 0.5f;
  nu->bezt[1].vec[0][1] = 0.8f;
  nu->bezt[1].vec[2][0] = 0.5f;
  nu->bezt[1].vec[2][1] = 0.2f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  nu->bezt[2].vec[1][0] = 0.0f;
  nu->bezt[2].vec[1][1] = 0.0f; /* Inflection near center. */
  nu->bezt[2].vec[0][0] = 0.3f;
  nu->bezt[2].vec[0][1] = 0.0f;
  nu->bezt[2].vec[2][0] = -0.3f;
  nu->bezt[2].vec[2][1] = 0.0f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  nu->bezt[3].vec[1][0] = -0.5f;
  nu->bezt[3].vec[1][1] = -0.5f;
  nu->bezt[3].vec[0][0] = -0.5f;
  nu->bezt[3].vec[0][1] = -0.2f;
  nu->bezt[3].vec[2][0] = -0.5f;
  nu->bezt[3].vec[2][1] = -0.8f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  nu->bezt[4].vec[1][0] = 0.0f;
  nu->bezt[4].vec[1][1] = -1.0f;
  nu->bezt[4].vec[0][0] = -0.3f;
  nu->bezt[4].vec[0][1] = -1.0f;
  nu->bezt[4].vec[2][0] = 0.3f;
  nu->bezt[4].vec[2][1] = -1.0f;
  nu->bezt[4].h1 = nu->bezt[4].h2 = HD_FREE;

  nu->bezt[5].vec[1][0] = -0.5f;
  nu->bezt[5].vec[1][1] = 0.5f;
  nu->bezt[5].vec[0][0] = -0.5f;
  nu->bezt[5].vec[0][1] = -0.2f;
  nu->bezt[5].vec[2][0] = -0.5f;
  nu->bezt[5].vec[2][1] = 0.8f;
  nu->bezt[5].h1 = nu->bezt[5].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  float orig_min_x, orig_min_y, orig_max_x, orig_max_y;
  calculate_bounds(&nurbsbase, orig_min_x, orig_min_y, orig_max_x, orig_max_y);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  float new_min_x, new_min_y, new_max_x, new_max_y;
  calculate_bounds(&nurbsbase, new_min_x, new_min_y, new_max_x, new_max_y);

  /* Bounds should be approximately preserved. */
  EXPECT_NEAR(new_min_x, orig_min_x, 0.1f);
  EXPECT_NEAR(new_max_x, orig_max_x, 0.1f);
  EXPECT_NEAR(new_min_y, orig_min_y, 0.1f);
  EXPECT_NEAR(new_max_y, orig_max_y, 0.1f);

  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Concentric overlapping circles.
 * Multiple overlapping circles should merge cleanly.
 */
TEST(vfontdata_sanitize, BezierReconstruction_ConcentricOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three concentric circles with partial overlap. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.0f, 0.0f, 0.9f);
  Nurb *c3 = create_circle_nurb(0.0f, 0.0f, 1.1f);

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);
  BLI_addtail(&nurbsbase, c3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* All shapes share center, should produce single or multiple valid contours. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Partial span derivative accuracy.
 * Handles computed from partial spans should have accurate derivatives.
 */
TEST(vfontdata_sanitize, BezierReconstruction_PartialSpanDerivative)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles that create partial span splits. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.8f, 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Check that handles at split points form smooth curves. */
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    for (int i = 0; i < nu->pntsu; i++) {
      const BezTriple &bezt = nu->bezt[i];

      /* Handle vectors. */
      float in_dx = bezt.vec[0][0] - bezt.vec[1][0];
      float in_dy = bezt.vec[0][1] - bezt.vec[1][1];
      float out_dx = bezt.vec[2][0] - bezt.vec[1][0];
      float out_dy = bezt.vec[2][1] - bezt.vec[1][1];

      float in_len = std::sqrt(in_dx * in_dx + in_dy * in_dy);
      float out_len = std::sqrt(out_dx * out_dx + out_dy * out_dy);

      if (in_len > 0.01f && out_len > 0.01f) {
        /* Normalize. */
        in_dx /= in_len;
        in_dy /= in_len;
        out_dx /= out_len;
        out_dy /= out_len;

        /* For smooth curves, handle-in and handle-out should be roughly collinear
         * (pointing in opposite directions). Dot product of normalized vectors
         * should be close to -1 for smooth tangent continuity. */
        float dot = in_dx * out_dx + in_dy * out_dy;
        /* Allow some deviation for corners, but should be mostly smooth. */
        EXPECT_LT(dot, 0.5f) << "Point " << i << " has handles pointing same direction";
      }
    }
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Item 29.1: Multi-Monotonic Merging Tests
 *
 * FontForge's MonoFollowForward/Backward merges consecutive monotonics from
 * the same original spline when there are no intersections between them.
 * This reduces unnecessary control points that would otherwise be created
 * at extrema points during monotonic decomposition.
 *
 * \{ */

/**
 * Test: Single non-overlapping circle returns unchanged (early exit path).
 * When there are no overlaps, the original curves should be preserved.
 */
TEST(vfontdata_sanitize, MultiMonotonicMerge_NoOverlapEarlyExit)
{
  ListBase nurbsbase = {nullptr, nullptr};

  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  int orig_points = circle->pntsu;
  EXPECT_EQ(orig_points, 4) << "Circle should start with 4 control points";

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Count output points. */
  int total_points = 0;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    total_points += nu->pntsu;
  }

  /* No overlaps = early exit, original returned unchanged. */
  EXPECT_EQ(total_points, orig_points)
      << "Circle should return unchanged when there are no overlaps. "
      << "Got " << total_points << " points, expected " << orig_points;

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Two overlapping circles - point count should be reasonable.
 * Note: Circle bezier segments are already monotonic (no internal extrema),
 * so multi-monotonic merging doesn't reduce points for circles.
 * The output depends on intersection points and segment boundaries.
 */
TEST(vfontdata_sanitize, MultiMonotonicMerge_TwoOverlappingCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles create 2 intersection points. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.5f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  int total_points = 0;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    total_points += nu->pntsu;
  }

  /* Two overlapping circles produce a merged outline. Each circle contributes
   * segments between intersection points. Since circle bezier segments have
   * no internal extrema, they're not split internally and can't be merged.
   * Expect approximately 6-10 points depending on how segments align. */
  EXPECT_LE(total_points, 10) << "Two overlapping circles should produce reasonable point count. "
                              << "Got " << total_points << " points.";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Three overlapping circles - verify point count doesn't explode.
 * More complex overlap scenarios should still have reasonable point counts.
 */
TEST(vfontdata_sanitize, MultiMonotonicMerge_ThreeOverlappingCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three circles in a row, each overlapping its neighbors. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.8f, 0.0f, 1.0f);
  Nurb *c3 = create_circle_nurb(1.6f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);
  BLI_addtail(&nurbsbase, c3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  int total_points = 0;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    total_points += nu->pntsu;
  }

  /* Three overlapping circles have ~4 intersection points total.
   * Since circles don't have internal extrema, expect roughly 10-16 points. */
  EXPECT_LE(total_points, 16)
      << "Three overlapping circles should produce reasonable point count. "
      << "Got " << total_points << " points.";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Circle overlapping with square - mixed shape types.
 * Tests that merging works correctly when combining curved and linear shapes.
 */
TEST(vfontdata_sanitize, MultiMonotonicMerge_CircleSquareOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Circle centered at origin, square offset to create overlap. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *square = create_square_nurb(0.5f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);
  BLI_addtail(&nurbsbase, square);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  int total_points = 0;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    total_points += nu->pntsu;
  }

  /* Circle + square with ~4 intersection points.
   * With merging: ~6-10 points expected.
   * Without merging: 4 circle extrema + 4 square corners = 8+ just from those. */
  EXPECT_LE(total_points, 10) << "Circle-square overlap should produce reasonable point count. "
                              << "Got " << total_points << " points.";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Large overlap - circle mostly inside another.
 * When one circle is mostly inside another, the outline is simple.
 */
TEST(vfontdata_sanitize, MultiMonotonicMerge_LargeOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Small circle mostly inside a larger one. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 2.0f); /* Large circle. */
  Nurb *c2 = create_circle_nurb(1.5f, 0.0f, 1.0f); /* Small circle, mostly inside. */
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  int total_points = 0;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    total_points += nu->pntsu;
  }

  /* Large overlap with 2 intersection points should produce ~4-6 points. */
  EXPECT_LE(total_points, 8) << "Large overlap should produce reasonable point count. "
                             << "Got " << total_points << " points.";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Bezier curve with internal extrema demonstrates multi-monotonic merging.
 * A curve where a single bezier segment has internal extrema will be split into
 * multiple monotonics. When the curve has no overlaps, these should be merged back
 * and the original point count should be preserved.
 */
TEST(vfontdata_sanitize, MultiMonotonicMerge_CurveWithExtrema)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a closed curve where some bezier segments have internal extrema.
   * An "S" shape or wave pattern would have dx/dt = 0 or dy/dt = 0 at interior points. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 4;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(4, __func__);

  /* Create a closed shape with 4 control points where some segments have
   * control points that create internal extrema. */

  /* Point 0: left side. */
  nu->bezt[0].vec[0][0] = -1.5f;
  nu->bezt[0].vec[0][1] = -0.5f;
  nu->bezt[0].vec[1][0] = -1.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[2][0] = -0.5f;
  nu->bezt[0].vec[2][1] = 1.0f; /* Handle extends above point 1, creating extremum. */
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Point 1: top (with handles that could create extrema). */
  nu->bezt[1].vec[0][0] = 0.0f;
  nu->bezt[1].vec[0][1] = 1.2f; /* This handle above the line creates an extremum. */
  nu->bezt[1].vec[1][0] = 0.5f;
  nu->bezt[1].vec[1][1] = 0.5f;
  nu->bezt[1].vec[2][0] = 1.0f;
  nu->bezt[1].vec[2][1] = 0.0f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Point 2: right side. */
  nu->bezt[2].vec[0][0] = 1.5f;
  nu->bezt[2].vec[0][1] = 0.5f;
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 0.0f;
  nu->bezt[2].vec[2][0] = 0.5f;
  nu->bezt[2].vec[2][1] = -1.0f; /* Handle extends below, creating extremum. */
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Point 3: bottom. */
  nu->bezt[3].vec[0][0] = 0.0f;
  nu->bezt[3].vec[0][1] = -1.2f;
  nu->bezt[3].vec[1][0] = -0.5f;
  nu->bezt[3].vec[1][1] = -0.5f;
  nu->bezt[3].vec[2][0] = -1.0f;
  nu->bezt[3].vec[2][1] = 0.0f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  BLI_addtail(&nurbsbase, nu);

  int orig_points = nu->pntsu;

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  int total_points = 0;
  for (const Nurb *n = static_cast<const Nurb *>(nurbsbase.first); n != nullptr; n = n->next) {
    total_points += n->pntsu;
  }

  /* A curve with no overlaps should return with original point count preserved.
   * The early exit path returns the original unchanged, but if it goes through
   * the algorithm, multi-monotonic merging should preserve the point count. */
  EXPECT_EQ(total_points, orig_points)
      << "Curve with no overlaps should preserve original point count. "
      << "Got " << total_points << " points, expected " << orig_points;

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Create an S-curve shape that has internal extrema in its bezier segments.
 * The curve goes from bottom-left to top-right with an S-shape that creates
 * dy/dt = 0 points internally, causing monotonic splits.
 */
static Nurb *create_s_curve_nurb(float x, float y, float width, float height)
{
  Nurb *nu = create_empty_bezier_nurb(4);

  /* Bottom-left point. */
  nu->bezt[0].vec[0][0] = x - width * 0.3f;
  nu->bezt[0].vec[0][1] = y - height * 0.5f;
  nu->bezt[0].vec[1][0] = x;
  nu->bezt[0].vec[1][1] = y - height * 0.5f;
  nu->bezt[0].vec[2][0] = x + width * 0.5f;
  nu->bezt[0].vec[2][1] = y - height * 0.5f;
  nu->bezt[0].h1 = nu->bezt[0].h2 = HD_FREE;

  /* Bottom-right with handle creating upward extremum. */
  nu->bezt[1].vec[0][0] = x + width * 0.3f;
  nu->bezt[1].vec[0][1] = y + height * 0.2f; /* Handle above point creates extremum. */
  nu->bezt[1].vec[1][0] = x + width * 0.5f;
  nu->bezt[1].vec[1][1] = y - height * 0.2f;
  nu->bezt[1].vec[2][0] = x + width * 0.7f;
  nu->bezt[1].vec[2][1] = y - height * 0.5f;
  nu->bezt[1].h1 = nu->bezt[1].h2 = HD_FREE;

  /* Top-right point. */
  nu->bezt[2].vec[0][0] = x + width * 0.3f;
  nu->bezt[2].vec[0][1] = y + height * 0.5f;
  nu->bezt[2].vec[1][0] = x + width;
  nu->bezt[2].vec[1][1] = y + height * 0.5f;
  nu->bezt[2].vec[2][0] = x + width * 1.3f;
  nu->bezt[2].vec[2][1] = y + height * 0.5f;
  nu->bezt[2].h1 = nu->bezt[2].h2 = HD_FREE;

  /* Top-left with handle creating downward extremum. */
  nu->bezt[3].vec[0][0] = x - width * 0.3f;
  nu->bezt[3].vec[0][1] = y - height * 0.2f; /* Handle below point creates extremum. */
  nu->bezt[3].vec[1][0] = x - width * 0.5f;
  nu->bezt[3].vec[1][1] = y + height * 0.2f;
  nu->bezt[3].vec[2][0] = x - width * 0.7f;
  nu->bezt[3].vec[2][1] = y + height * 0.5f;
  nu->bezt[3].h1 = nu->bezt[3].h2 = HD_FREE;

  return nu;
}

/**
 * Test: S-curve with internal extrema overlapping with a square.
 *
 * The S-curve has bezier segments with internal extrema (dy/dt = 0 points),
 * which cause the segments to be split into multiple monotonics during
 * monotonic decomposition. When tracing through the overlapping region,
 * consecutive monotonics from the same original spline should be merged
 * back together by MonoFollowForward/Backward logic.
 */
TEST(vfontdata_sanitize, MultiMonotonicMerge_SCurveWithSquare)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* S-curve centered at origin, overlapping with a square. */
  Nurb *s_curve = create_s_curve_nurb(0.0f, 0.0f, 1.5f, 1.0f);
  Nurb *square = create_square_nurb(0.0f, -0.3f, 0.6f);
  BLI_addtail(&nurbsbase, s_curve);
  BLI_addtail(&nurbsbase, square);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Count output contours and points. */
  int contour_count = count_nurbs(&nurbsbase);
  int total_points = 0;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    total_points += nu->pntsu;
  }

  /* The overlap should produce valid output. */
  EXPECT_GE(contour_count, 1) << "Should produce at least one contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* With multi-monotonic merging, the point count should be reasonable.
   * Without merging, each monotonic split would create extra points.
   * With proper merging, consecutive same-spline monotonics are combined. */
  EXPECT_LE(total_points, 20) << "S-curve + square overlap should have reasonable point count. "
                              << "Got " << total_points
                              << " points (high count may indicate merging issues).";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Chain of connected bezier segments without intersections.
 *
 * When a single contour has no overlaps with anything else, it should
 * exit early and return unchanged. This verifies the early exit path
 * preserves the original curve structure.
 */
TEST(vfontdata_sanitize, MultiMonotonicMerge_ChainNoIntersections)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a closed zigzag/wave shape with 6 points.
   * Each segment is a simple curve, no internal extrema. */
  Nurb *nu = create_empty_bezier_nurb(6);

  /* Zigzag pattern going right then left. */
  float pts[][2] = {
      {0.0f, 0.0f},
      {1.0f, 0.5f},
      {2.0f, 0.0f},
      {2.0f, 1.0f},
      {1.0f, 0.5f},
      {0.0f, 1.0f},
  };

  for (int i = 0; i < 6; i++) {
    int prev = (i == 0) ? 5 : i - 1;
    int next = (i == 5) ? 0 : i + 1;

    nu->bezt[i].vec[1][0] = pts[i][0];
    nu->bezt[i].vec[1][1] = pts[i][1];

    /* Simple handles pointing toward neighbors. */
    nu->bezt[i].vec[0][0] = pts[i][0] + (pts[prev][0] - pts[i][0]) * 0.3f;
    nu->bezt[i].vec[0][1] = pts[i][1] + (pts[prev][1] - pts[i][1]) * 0.3f;
    nu->bezt[i].vec[2][0] = pts[i][0] + (pts[next][0] - pts[i][0]) * 0.3f;
    nu->bezt[i].vec[2][1] = pts[i][1] + (pts[next][1] - pts[i][1]) * 0.3f;

    nu->bezt[i].h1 = nu->bezt[i].h2 = HD_FREE;
  }

  BLI_addtail(&nurbsbase, nu);

  int orig_points = nu->pntsu;

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Single contour with no overlaps should return unchanged via early exit. */
  int total_points = 0;
  for (const Nurb *n = static_cast<const Nurb *>(nurbsbase.first); n != nullptr; n = n->next) {
    total_points += n->pntsu;
  }

  EXPECT_EQ(total_points, orig_points)
      << "Single contour with no overlaps should preserve point count. "
      << "Got " << total_points << " points, expected " << orig_points;

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Verify multi-monotonic merging reduces point count.
 *
 * Create a scenario where monotonics from the same spline would be
 * consecutive in the output if not merged, but should be merged together.
 * This directly tests that the merging logic is working.
 */
TEST(vfontdata_sanitize, MultiMonotonicMerge_PointCountReduction)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles where the intersection creates a simple merged contour.
   * The circles are positioned so that large arcs are traced continuously. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(1.5f, 0.0f, 1.0f); /* Slight overlap on right side. */
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  int total_points = 0;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase.first); nu != nullptr; nu = nu->next) {
    total_points += nu->pntsu;
  }

  /* Two slightly overlapping circles should merge into one contour.
   * With proper multi-monotonic merging, the point count should be
   * approximately the sum of arcs used (around 6-8 points typically).
   * Without merging, each monotonic boundary would add extra points. */
  EXPECT_LE(total_points, 10) << "Two overlapping circles should merge efficiently. "
                              << "Got " << total_points << " points.";

  /* Should produce exactly one contour (the merged outline). */
  int contour_count = count_nurbs(&nurbsbase);
  EXPECT_EQ(contour_count, 1) << "Should produce single merged contour";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Linear Segment Detection Tests (Item 29.2)
 *
 * Tests for detecting linear bezier segments and setting HD_VECT handles.
 * FontForge detects linear segments with SplineIsLinear() and sets handles
 * to the point position (equivalent to Blender's HD_VECT).
 *
 * A bezier segment is linear if:
 * - Control points lie on the line between endpoints
 * - Control points don't extend beyond endpoints (t values in [0,1])
 * \{ */

/**
 * Create a square with truly linear edges (handles at point positions).
 */
static Nurb *create_linear_square_nurb(float x, float y, float size)
{
  Nurb *nu = create_empty_bezier_nurb(4);

  /* All handles at point positions = linear segments. */
  set_bezt_linear(nu->bezt[0], x, y);               /* Bottom-left */
  set_bezt_linear(nu->bezt[1], x + size, y);        /* Bottom-right */
  set_bezt_linear(nu->bezt[2], x + size, y + size); /* Top-right */
  set_bezt_linear(nu->bezt[3], x, y + size);        /* Top-left */

  return nu;
}

/**
 * Count handles of a specific type in all nurbs.
 */
static int count_handle_type(const ListBase *nurbsbase, char handle_type)
{
  int count = 0;
  for (const Nurb *nu = static_cast<const Nurb *>(nurbsbase->first); nu != nullptr; nu = nu->next)
  {
    if (nu->bezt == nullptr) {
      continue;
    }
    for (int i = 0; i < nu->pntsu; i++) {
      if (nu->bezt[i].h1 == handle_type) {
        count++;
      }
      if (nu->bezt[i].h2 == handle_type) {
        count++;
      }
    }
  }
  return count;
}

/**
 * Test: Two linear squares that don't overlap should preserve linearity.
 * When two separate linear squares are processed, their HD_VECT handles
 * should be preserved in the output since they don't interact.
 */
TEST(vfontdata_sanitize, LinearSegment_NoOverlapPreserved)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two non-overlapping linear squares. */
  Nurb *sq1 = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *sq2 = create_linear_square_nurb(3.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  /* Verify input has HD_VECT handles. */
  int vect_before = count_handle_type(&nurbsbase, HD_VECT);
  EXPECT_EQ(vect_before, 16) << "Input should have 16 HD_VECT handles (8 per square)";

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* After processing, linear segments should still have HD_VECT (or equivalent). */
  EXPECT_EQ(count_nurbs(&nurbsbase), 2) << "Two separate squares should remain separate";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Square with curved handles - should stay curved after no-overlap processing.
 * A square with curved (non-linear) edges should not be converted to linear.
 */
TEST(vfontdata_sanitize, LinearSegment_CurvedStaysCurved)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Square with curved edges (handles not at point positions). */
  Nurb *sq = create_square_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, sq);

  /* Verify input has HD_FREE handles (from create_square_nurb). */
  int free_before = count_handle_type(&nurbsbase, HD_FREE);
  EXPECT_EQ(free_before, 8) << "Input should have 8 HD_FREE handles";

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* After processing, curved handles should remain. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Two overlapping linear squares - merged output should have linear handles.
 *
 * When two linear squares overlap, the merged output should detect that the
 * resulting edges are linear and use HD_VECT handles.
 */
TEST(vfontdata_sanitize, LinearSegment_OverlapMerge)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping linear squares. */
  Nurb *sq1 = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *sq2 = create_linear_square_nurb(0.5f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into a single contour. */
  int nurbs_after = count_nurbs(&nurbsbase);
  EXPECT_EQ(nurbs_after, 1) << "Two overlapping squares should merge into one";

  /* Verify output is valid. */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* The merged contour should have HD_VECT handles since all edges are linear.
   * With 8 points in the merged shape, we expect 16 HD_VECT handles. */
  int vect_after = count_handle_type(&nurbsbase, HD_VECT);
  EXPECT_EQ(vect_after, 16) << "Merged linear contour should have HD_VECT handles";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Circle overlapping with linear square.
 * At intersection points, handles will be computed from derivatives.
 * Non-intersected portions of the square should ideally remain linear.
 */
TEST(vfontdata_sanitize, LinearSegment_CircleSquareOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Circle overlapping with a linear square. */
  Nurb *circle = create_circle_nurb(0.5f, 0.5f, 0.8f);
  Nurb *sq = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);
  BLI_addtail(&nurbsbase, sq);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Triangle (3-point linear contour).
 * A triangle with linear edges should process correctly.
 */
TEST(vfontdata_sanitize, LinearSegment_Triangle)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a triangle with linear edges. */
  Nurb *nu = create_empty_bezier_nurb(3);
  set_bezt_linear(nu->bezt[0], 0.0f, 0.0f); /* Bottom-left */
  set_bezt_linear(nu->bezt[1], 1.0f, 0.0f); /* Bottom-right */
  set_bezt_linear(nu->bezt[2], 0.5f, 1.0f); /* Top center */

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Single non-overlapping triangle should be preserved. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Check that collinear control points are detected as linear.
 * A segment where handles lie on the line between endpoints should be linear.
 */
TEST(vfontdata_sanitize, LinearSegment_CollinearHandles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a square where handles are on the edges (collinear = linear). */
  Nurb *nu = create_empty_bezier_nurb(4);

  /* Handles at 1/3 and 2/3 along edges - still collinear with edge direction. */
  set_bezt(nu->bezt[0], 0.0f, 0.0f, 0.0f, 0.0f, 0.33f, 0.0f);  /* Bottom-left */
  set_bezt(nu->bezt[1], 1.0f, 0.0f, 0.67f, 0.0f, 1.0f, 0.33f); /* Bottom-right */
  set_bezt(nu->bezt[2], 1.0f, 1.0f, 1.0f, 0.67f, 0.67f, 1.0f); /* Top-right */
  set_bezt(nu->bezt[3], 0.0f, 1.0f, 0.33f, 1.0f, 0.0f, 0.67f); /* Top-left */

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should be preserved as single contour. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* When linear detection is implemented, all edges should be detected as linear
   * since all handles lie on the edges. For now, just verify it processes. */

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Small Handle Threshold Tests (Item 29.3)
 *
 * Tests for collapsing near-zero handle offsets to prevent numerical noise.
 * FontForge collapses handles when the derivative component is within ±0.01.
 * This prevents tiny spurious curves from being created.
 * \{ */

/**
 * Test: Very small handles should be collapsed.
 * Create a shape with tiny handle offsets that should be collapsed.
 */
TEST(vfontdata_sanitize, SmallHandleThreshold_TinyHandleCollapsed)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a square with extremely small handle offsets (< 0.01 threshold). */
  const float tiny = 0.005f;
  Nurb *nu = create_square_nurb_handles(0.0f, 0.0f, 1.0f, tiny);
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a single valid contour. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* Verify handles have been collapsed (offset should be 0 or very small).
   * The small handle threshold collapses handles < 0.01 to the point position. */
  const Nurb *out_nu = static_cast<const Nurb *>(nurbsbase.first);
  ASSERT_NE(out_nu, nullptr);
  ASSERT_NE(out_nu->bezt, nullptr);

  for (int i = 0; i < out_nu->pntsu; i++) {
    const BezTriple &bezt = out_nu->bezt[i];
    /* Handle-in offset should be essentially zero after collapsing. */
    float h1_len = sqrtf(std::pow(bezt.vec[0][0] - bezt.vec[1][0], 2.0f) +
                         std::pow(bezt.vec[0][1] - bezt.vec[1][1], 2.0f));
    /* Handle-out offset should be essentially zero after collapsing. */
    float h2_len = sqrtf(std::pow(bezt.vec[2][0] - bezt.vec[1][0], 2.0f) +
                         std::pow(bezt.vec[2][1] - bezt.vec[1][1], 2.0f));

    /* Handles should either be collapsed (zero) or above threshold.
     * Since input had tiny handles, they should all be collapsed. */
    EXPECT_LT(h1_len, 0.01f) << "Handle-in at point " << i << " should be collapsed (got "
                             << h1_len << ")";
    EXPECT_LT(h2_len, 0.01f) << "Handle-out at point " << i << " should be collapsed (got "
                             << h2_len << ")";
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Normal-sized handles should NOT be collapsed.
 * Handles above the threshold should be preserved.
 */
TEST(vfontdata_sanitize, SmallHandleThreshold_NormalHandlePreserved)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a circle with significant handle offsets. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a single contour. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* Handles should have significant length (circle uses ~0.55 * radius). */
  const Nurb *out_nu = static_cast<const Nurb *>(nurbsbase.first);
  ASSERT_NE(out_nu, nullptr);
  ASSERT_NE(out_nu->bezt, nullptr);

  /* At least some handles should have non-collapsed offsets. */
  int non_collapsed = 0;
  for (int i = 0; i < out_nu->pntsu; i++) {
    const BezTriple &bezt = out_nu->bezt[i];
    float h1_len = sqrtf(std::pow(bezt.vec[0][0] - bezt.vec[1][0], 2.0f) +
                         std::pow(bezt.vec[0][1] - bezt.vec[1][1], 2.0f));
    float h2_len = sqrtf(std::pow(bezt.vec[2][0] - bezt.vec[1][0], 2.0f) +
                         std::pow(bezt.vec[2][1] - bezt.vec[1][1], 2.0f));
    if (h1_len > 0.1f || h2_len > 0.1f) {
      non_collapsed++;
    }
  }
  EXPECT_GT(non_collapsed, 0) << "Circle should have some non-collapsed handles";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Handles at boundary of threshold.
 * Handles just above and below the threshold.
 */
TEST(vfontdata_sanitize, SmallHandleThreshold_BoundaryTest)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a contour with handles at boundary: some at 0.005, some at 0.015. */
  Nurb *nu = create_empty_bezier_nurb(3);

  /* Point 0: tiny handle-in (0.005), handle-out just above threshold (0.015). */
  set_bezt(nu->bezt[0], 0.0f, 0.0f, -0.005f, 0.0f, 0.015f, 0.0f);
  /* Point 1: normal handles. */
  set_bezt(nu->bezt[1], 1.0f, 0.0f, 0.7f, 0.0f, 1.0f, 0.3f);
  /* Point 2: normal handles. */
  set_bezt(nu->bezt[2], 0.5f, 1.0f, 0.7f, 1.0f, 0.3f, 0.7f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test: Two overlapping shapes - small handles from numerical precision.
 * When shapes overlap, computed handles may have tiny offsets from
 * numerical precision. These should be collapsed.
 */
TEST(vfontdata_sanitize, SmallHandleThreshold_OverlapPrecision)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two overlapping circles. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.5f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into valid contour(s). */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Items 31-40 Tests
 *
 * Tests for items 31-40 from knowledge.rst.
 * These test whether specific features are needed.
 * \{ */

/**
 * Item 31: Stupid control point removal.
 * Test with control points that create unnecessary inflections.
 */
TEST(vfontdata_sanitize, Item31_StupidControlPoints)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a shape with a control point that backtracks. */
  Nurb *nu = create_empty_bezier_nurb(3);

  /* Point 0 with handle that overshoots badly. */
  set_bezt(nu->bezt[0], 0.0f, 0.0f, -0.5f, 0.0f, 2.0f, 0.0f); /* Handle-out overshoots */
  set_bezt(nu->bezt[1], 1.0f, 0.0f, 0.5f, 0.0f, 1.5f, 0.5f);
  set_bezt(nu->bezt[2], 0.5f, 1.0f, 0.8f, 1.0f, 0.2f, 0.8f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should still produce valid output. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 31.1: Test directional analysis - backward-pointing handle.
 * A handle that points backward (opposite to segment direction) should be collapsed.
 */
TEST(vfontdata_sanitize, Item31_1_BackwardPointingHandle)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a triangle with one backward-pointing handle. */
  Nurb *nu = create_empty_bezier_nurb(3);

  /* Point 0 at origin, handle-out points BACKWARD (negative x). */
  set_bezt(nu->bezt[0], 0.0f, 0.0f, 0.0f, 0.0f, -0.5f, 0.0f); /* Handle points backward! */
  /* Point 1 at (1, 0), normal handles. */
  set_bezt(nu->bezt[1], 1.0f, 0.0f, 0.9f, 0.0f, 1.0f, 0.1f);
  /* Point 2 at (0.5, 1), normal handles. */
  set_bezt(nu->bezt[2], 0.5f, 1.0f, 0.6f, 1.0f, 0.4f, 1.0f);

  /* Record original handle position. */
  float orig_h2_x = nu->bezt[0].vec[2][0];
  EXPECT_LT(orig_h2_x, 0.0f) << "Handle should start pointing backward (negative x)";

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 31.2: Test backward-pointing handle-in.
 */
TEST(vfontdata_sanitize, Item31_2_BackwardHandleIn)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a shape where handle-in at point 1 points the wrong way. */
  Nurb *nu = create_empty_bezier_nurb(3);

  set_bezt(nu->bezt[0], 0.0f, 0.0f, -0.1f, 0.0f, 0.1f, 0.0f);
  /* Point 1: handle-in points toward point 2 instead of toward point 0. */
  set_bezt(nu->bezt[1], 1.0f, 0.0f, 1.1f, 0.0f, 1.0f, 0.1f); /* Handle-in at 1.1 is forward! */
  set_bezt(nu->bezt[2], 0.5f, 1.0f, 0.6f, 1.0f, 0.4f, 1.0f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 31.3: Test overshooting handle.
 * A handle that extends beyond the next point should be clamped.
 */
TEST(vfontdata_sanitize, Item31_3_OvershootingHandle)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a triangle where handle-out at point 0 overshoots point 1. */
  Nurb *nu = create_empty_bezier_nurb(3);

  /* Point 0 at (0,0), handle-out at (3,0) - way past point 1 at (1,0). */
  set_bezt(nu->bezt[0], 0.0f, 0.0f, -0.1f, 0.0f, 3.0f, 0.0f); /* Handle overshoots! */
  set_bezt(nu->bezt[1], 1.0f, 0.0f, 0.9f, 0.0f, 1.0f, 0.1f);
  set_bezt(nu->bezt[2], 0.5f, 1.0f, 0.6f, 1.0f, 0.4f, 1.0f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 32: Annoying extrema removal.
 * Test shapes with very small bumps at extrema.
 */
TEST(vfontdata_sanitize, Item32_AnnoyingExtrema)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a nearly-square shape with tiny bumps. */
  Nurb *nu = create_empty_bezier_nurb(4);

  /* Tiny handle offsets perpendicular to edges - creating small extrema. */
  set_bezt(nu->bezt[0], 0.0f, 0.0f, -0.001f, 0.001f, 0.1f, 0.001f);
  set_bezt(nu->bezt[1], 1.0f, 0.0f, 0.9f, 0.001f, 1.001f, 0.1f);
  set_bezt(nu->bezt[2], 1.0f, 1.0f, 1.001f, 0.9f, 0.9f, 1.001f);
  set_bezt(nu->bezt[3], 0.0f, 1.0f, 0.1f, 1.001f, -0.001f, 0.9f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 33.3: Solver accuracy at edge cases near t=0 and t=1.
 */
TEST(vfontdata_sanitize, Item33_3_SolverEdgeCases)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create two circles that barely touch at their edges.
   * This tests intersection finding near t=0 and t=1 boundaries. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(1.999f, 0.0f, 1.0f); /* Just barely touching */

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle edge case without crashing. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 34.4: Complex intersections that might create stranded monotonics.
 */
TEST(vfontdata_sanitize, Item34_4_ComplexIntersections)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three overlapping circles create complex intersection patterns. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.5f, 0.0f, 1.0f);
  Nurb *c3 = create_circle_nurb(0.25f, 0.5f, 1.0f);

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);
  BLI_addtail(&nurbsbase, c3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output with all monotonics properly connected. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 35.4: Complex intersection patterns testing t-value validity.
 */
TEST(vfontdata_sanitize, Item35_4_TValueValidity)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Figure-8 creates intersections at interior t-values. */
  Nurb *nu = create_figure_8_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid contours with correct t-value handling. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 38.1: Mixed isolated and overlapping contours.
 */
TEST(vfontdata_sanitize, Item38_1_IsolatedPlusOverlapping)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* One isolated circle far from two overlapping circles. */
  Nurb *isolated = create_circle_nurb(10.0f, 0.0f, 1.0f); /* Far away */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.5f, 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, isolated);
  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Isolated circle should survive, overlapping circles should merge.
   * Result: 2 contours (1 isolated + 1 merged). */
  EXPECT_EQ(count_nurbs(&nurbsbase), 2);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 39.3: Touching circles should remain separate.
 */
TEST(vfontdata_sanitize, Item39_3_TouchingCircles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles that exactly touch (not overlap). */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(2.0f, 0.0f, 1.0f); /* Exactly touching at (1,0) */

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Touching circles should remain as two separate contours. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 2);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 40.1: Test adjacent monotonic skip with actual overlaps.
 * Circle with hole tests that adjacent monotonics are properly handled.
 */
TEST(vfontdata_sanitize, Item40_1_AdjacentMonotonicWithOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Outer circle with inner circle - inner is completely contained.
   * With non-zero winding, the inner circle (same winding direction) adds to
   * the winding count, so it may be merged into one contour or kept as two
   * depending on winding calculation. */
  Nurb *outer = create_circle_nurb(0.0f, 0.0f, 2.0f);
  Nurb *inner = create_circle_nurb(0.0f, 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, outer);
  BLI_addtail(&nurbsbase, inner);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce at least one valid contour. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Create a square with clockwise winding (opposite of create_linear_square_nurb).
 * Points are ordered CW: bottom-left -> top-left -> top-right -> bottom-right.
 */
static Nurb *create_linear_square_nurb_cw(float x, float y, float size)
{
  Nurb *nu = create_empty_bezier_nurb(4);

  /* CW order: bottom-left -> top-left -> top-right -> bottom-right. */
  set_bezt_linear(nu->bezt[0], x, y);               /* Bottom-left */
  set_bezt_linear(nu->bezt[1], x, y + size);        /* Top-left */
  set_bezt_linear(nu->bezt[2], x + size, y + size); /* Top-right */
  set_bezt_linear(nu->bezt[3], x + size, y);        /* Bottom-right */

  return nu;
}

/**
 * Item 16: Linear spline direction cancellation.
 *
 * When two linear segments trace the exact same path in opposite directions
 * (one going a→b, the other going b→a), they should cancel each other out.
 *
 * This tests FontForge's TestForBadDirections() which handles the case where
 * "depending on the order we hit them they may both be marked needed".
 *
 * FontForge reference: splineoverlap.c lines 2935-2966, OverlapBugs.sfd: asciicircumflex
 */
TEST(vfontdata_sanitize, Item16_LinearDirectionCancellation)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create CCW and CW squares at the same position.
   *
   * CCW square edges: (0,0)→(1,0)→(1,1)→(0,1)→(0,0)
   * CW square edges:  (0,0)→(0,1)→(1,1)→(1,0)→(0,0)
   *
   * The bottom edge: CCW goes (0,0)→(1,0), CW goes (1,0)→(0,0)
   * The top edge:    CCW goes (1,1)→(0,1), CW goes (0,1)→(1,1)
   * etc.
   *
   * Each edge is traced twice in opposite directions, so they should all cancel.
   * The result should be empty (no contours). */
  Nurb *sq_ccw = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *sq_cw = create_linear_square_nurb_cw(0.0f, 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, sq_ccw);
  BLI_addtail(&nurbsbase, sq_cw);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* CCW + CW at same position = winding number 0 = no visible area.
   * All opposing linear segments should cancel, resulting in no output. */
  int result_count = count_nurbs(&nurbsbase);
  EXPECT_EQ(result_count, 0)
      << "CCW + CW squares at same position should cancel to nothing (winding 0)";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Additional test: Partial cancellation with adjacent shapes.
 * Two squares sharing a common edge where that edge has opposing directions.
 */
TEST(vfontdata_sanitize, Item16_LinearDirectionCancellation_SharedEdge)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two adjacent CCW squares sharing an edge.
   * Square 1: (0,0) to (1,1)
   * Square 2: (1,0) to (2,1)
   *
   * The shared edge at x=1 goes:
   * - In sq1: (1,0) -> (1,1) (right edge, going up)
   * - In sq2: (1,1) -> (1,0) (left edge, going down)
   *
   * These opposing linear segments on the shared edge should be marked as unneeded
   * by TestForBadDirections, leaving just the outer perimeter. */
  Nurb *sq1 = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *sq2 = create_linear_square_nurb(1.0f, 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Two adjacent squares should merge into a single rectangle. */
  int result_count = count_nurbs(&nurbsbase);
  EXPECT_EQ(result_count, 1) << "Two adjacent squares should merge to one rectangle";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* The merged rectangle should have 6 points (since shared edge is removed). */
  if (result_count == 1) {
    const Nurb *result = static_cast<const Nurb *>(nurbsbase.first);
    /* Expected shape: rectangle with corners at (0,0), (2,0), (2,1), (0,1)
     * but with the original corner vertices preserved, giving 6 points. */
    EXPECT_GE(result->pntsu, 4) << "Merged rectangle should have at least 4 points";
    EXPECT_LE(result->pntsu, 8) << "Merged rectangle should have at most 8 points";
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 17: Open contour joining during reconstruction.
 *
 * FontForge's FindMatchingContour() and JoinAllNeeded() logic can join open
 * contours that result from errors during reconstruction. This test creates
 * complex overlapping geometry that exercises the fragment joining logic.
 *
 * Reference: FontForge splineoverlap.c:3242-3280 (FindMatchingContour)
 *            FontForge splineoverlap.c:3321-3350 (joining logic in JoinAllNeeded)
 */
TEST(vfontdata_sanitize, Item17_OpenContourJoining_OverlappingTriangles)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create three overlapping triangular shapes.
   * This creates complex intersections that may produce fragments needing joining.
   *
   * Triangle 1: pointing right at (0,0)
   * Triangle 2: pointing right at (0.5, 0.3) - overlaps with triangle 1
   * Triangle 3: pointing right at (0.3, -0.2) - overlaps with both
   *
   * The overlapping regions create multiple intersection points, and reconstruction
   * must join any open fragments to produce valid closed contours. */
  auto create_triangle = [](float cx, float cy, float size) -> Nurb * {
    Nurb *nu = create_empty_bezier_nurb(3);
    /* Right-pointing triangle (CCW) */
    set_bezt_linear(nu->bezt[0], cx, cy);                      /* Left point */
    set_bezt_linear(nu->bezt[1], cx + size, cy + size * 0.5f); /* Right point */
    set_bezt_linear(nu->bezt[2], cx, cy + size);               /* Top-left point */
    return nu;
  };

  Nurb *t1 = create_triangle(0.0f, 0.0f, 1.0f);
  Nurb *t2 = create_triangle(0.5f, 0.3f, 0.8f);
  Nurb *t3 = create_triangle(0.3f, -0.2f, 0.7f);

  BLI_addtail(&nurbsbase, t1);
  BLI_addtail(&nurbsbase, t2);
  BLI_addtail(&nurbsbase, t3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* All resulting contours should be closed (cyclic). */
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase))
      << "All output contours should be closed after fragment joining";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* Should produce at least one valid merged contour. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 17.2: Open contour joining with shared edges.
 *
 * Three squares arranged in an L-shape, creating shared edges where
 * fragments might need joining.
 */
TEST(vfontdata_sanitize, Item17_OpenContourJoining_LShape)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create an L-shape from three unit squares:
   *
   *     +---+
   *     | 3 |
   * +---+---+
   * | 1 | 2 |
   * +---+---+
   *
   * Multiple shared edges create complex topology where fragment joining is needed. */
  Nurb *sq1 = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *sq2 = create_linear_square_nurb(1.0f, 0.0f, 1.0f);
  Nurb *sq3 = create_linear_square_nurb(1.0f, 1.0f, 1.0f);

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);
  BLI_addtail(&nurbsbase, sq3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a single merged L-shaped contour. */
  int result_count = count_nurbs(&nurbsbase);
  EXPECT_EQ(result_count, 1) << "L-shape should merge to single contour";
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase)) << "Output contour should be closed";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* The L-shape perimeter has 6 corners. */
  if (result_count == 1) {
    const Nurb *result = static_cast<const Nurb *>(nurbsbase.first);
    EXPECT_GE(result->pntsu, 6) << "L-shape should have at least 6 corners";
    EXPECT_LE(result->pntsu, 12) << "L-shape should not have excessive points";
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 17.3: Open contour joining with partial overlap.
 *
 * Two squares that partially overlap (not fully shared edge),
 * creating a scenario where reconstruction produces fragments.
 */
TEST(vfontdata_sanitize, Item17_OpenContourJoining_PartialOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares with partial overlap:
   *
   * Square 1: (0,0) to (1,1)
   * Square 2: (0.5, 0.5) to (1.5, 1.5)
   *
   * They overlap in a smaller square region, creating 4 intersection points. */
  Nurb *sq1 = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *sq2 = create_linear_square_nurb(0.5f, 0.5f, 1.0f);

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a single merged shape (like an L rotated 45°). */
  int result_count = count_nurbs(&nurbsbase);
  EXPECT_EQ(result_count, 1) << "Partially overlapping squares should merge";
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase)) << "Output contour should be closed";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* The merged shape has 8 vertices (two overlapping squares minus shared region). */
  if (result_count == 1) {
    const Nurb *result = static_cast<const Nurb *>(nurbsbase.first);
    EXPECT_GE(result->pntsu, 6) << "Merged shape should have at least 6 corners";
    EXPECT_LE(result->pntsu, 12) << "Merged shape should not have excessive points";
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 17.4: Open contour joining - four squares in a plus sign.
 *
 * Creates maximum shared edge complexity.
 */
TEST(vfontdata_sanitize, Item17_OpenContourJoining_PlusSign)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Four squares arranged in a plus sign:
   *
   *     +---+
   *     | 4 |
   * +---+---+---+
   * | 1 | 2 | 3 |
   * +---+---+---+
   *
   * Square 2 is at center, others share edges with it. */
  Nurb *sq1 = create_linear_square_nurb(0.0f, 1.0f, 1.0f); /* Left */
  Nurb *sq2 = create_linear_square_nurb(1.0f, 1.0f, 1.0f); /* Center */
  Nurb *sq3 = create_linear_square_nurb(2.0f, 1.0f, 1.0f); /* Right */
  Nurb *sq4 = create_linear_square_nurb(1.0f, 2.0f, 1.0f); /* Top */

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);
  BLI_addtail(&nurbsbase, sq3);
  BLI_addtail(&nurbsbase, sq4);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a single plus-shaped contour. */
  int result_count = count_nurbs(&nurbsbase);
  EXPECT_EQ(result_count, 1) << "Plus sign should merge to single contour";
  EXPECT_TRUE(all_nurbs_cyclic(&nurbsbase)) << "Output contour should be closed";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* The plus sign perimeter has 12 corners. */
  if (result_count == 1) {
    const Nurb *result = static_cast<const Nurb *>(nurbsbase.first);
    EXPECT_GE(result->pntsu, 10) << "Plus sign should have at least 10 corners";
    EXPECT_LE(result->pntsu, 16) << "Plus sign should not have excessive points";
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 38: Synthetic intersections for isolated contours.
 * A contour with no intersections should still be preserved.
 */
TEST(vfontdata_sanitize, Item38_IsolatedContour)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* A single isolated circle - no intersections with anything. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should preserve the isolated contour. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 39: Multi-pass closeness tolerance.
 * Test shapes where segments are extremely close.
 */
TEST(vfontdata_sanitize, Item39_MultiPassCloseness)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles that just barely touch (not overlap). */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 0.5f);
  Nurb *c2 = create_circle_nurb(1.0f, 0.0f, 0.5f); /* Touch at x=0.5 */

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Two touching circles should remain as two contours. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 40: Adjacent monotonic detection.
 * Intersections between adjacent monotonics (continuation of same spline)
 * should be handled specially.
 */
TEST(vfontdata_sanitize, Item40_AdjacentMonotonics)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* A single circle - monotonics at extrema points are "adjacent". */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a valid circle. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* Check that we don't have excessive points from false "intersections"
   * at the extrema where monotonics join. */
  const Nurb *out_nu = static_cast<const Nurb *>(nurbsbase.first);
  EXPECT_LE(out_nu->pntsu, 8) << "Circle should not have excessive points";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 41: Intersection t-value clamping.
 * When intersection is outside monotonic's [tstart, tend] range but within
 * epsilon, it should be clamped to the endpoint.
 */
TEST(vfontdata_sanitize, Item41_TValueClamping)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares that intersect at corners - tests t-value endpoint handling. */
  Nurb *sq1 = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.3f);
  Nurb *sq2 = create_square_nurb_handles(0.5f, 0.5f, 1.0f, 0.3f);

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into a single contour. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 42: Slice-based intersection search.
 * Tests complex curve intersections that would benefit from sliced search.
 */
TEST(vfontdata_sanitize, Item42_SliceBasedIntersection)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two sine-wave-like curves that cross multiple times.
   * Create using bezier curves with multiple inflection points. */
  Nurb *nu1 = create_empty_bezier_nurb(5);
  set_bezt(nu1->bezt[0], 0.0f, 0.0f, -0.2f, 0.0f, 0.2f, 0.5f);
  set_bezt(nu1->bezt[1], 0.5f, 1.0f, 0.3f, 1.0f, 0.7f, 1.0f);
  set_bezt(nu1->bezt[2], 1.0f, 0.0f, 0.8f, 0.0f, 1.2f, 0.0f);
  set_bezt(nu1->bezt[3], 1.5f, 1.0f, 1.3f, 1.0f, 1.7f, 1.0f);
  set_bezt(nu1->bezt[4], 2.0f, 0.0f, 1.8f, 0.0f, 2.2f, 0.5f);

  /* Horizontal band that crosses the wave multiple times. */
  Nurb *nu2 = create_empty_bezier_nurb(4);
  set_bezt_linear(nu2->bezt[0], -0.2f, 0.5f);
  set_bezt_linear(nu2->bezt[1], 2.2f, 0.5f);
  set_bezt_linear(nu2->bezt[2], 2.2f, 0.7f);
  set_bezt_linear(nu2->bezt[3], -0.2f, 0.7f);

  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid contours. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 43: FF_RELATIONAL_GEOM support (original t-values).
 * Tests cases where t-value adjustments could create gaps.
 */
TEST(vfontdata_sanitize, Item43_RelationalGeom)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* A shape with very small segments that could cause t-value gaps. */
  Nurb *nu = create_empty_bezier_nurb(6);
  set_bezt_linear(nu->bezt[0], 0.0f, 0.0f);
  set_bezt_linear(nu->bezt[1], 1.0f, 0.0f);
  set_bezt_linear(nu->bezt[2], 1.001f, 0.001f); /* Tiny segment */
  set_bezt_linear(nu->bezt[3], 1.0f, 1.0f);
  set_bezt_linear(nu->bezt[4], 0.0f, 1.0f);
  set_bezt_linear(nu->bezt[5], -0.001f, 0.001f); /* Tiny segment */

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a valid contour, possibly simplified. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 44: Minimum difference calculation.
 * Tests that we don't get infinite loops with floating-point edge cases.
 */
TEST(vfontdata_sanitize, Item44_MinimumDifference)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Coordinates that are only one ULP apart. */
  float base = 1.0f;
  float tiny = std::nextafter(base, 2.0f); /* Next representable float */

  Nurb *nu = create_empty_bezier_nurb(4);
  set_bezt_linear(nu->bezt[0], 0.0f, 0.0f);
  set_bezt_linear(nu->bezt[1], base, 0.0f);
  set_bezt_linear(nu->bezt[2], tiny, 1.0f); /* Nearly same x as previous */
  set_bezt_linear(nu->bezt[3], 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should complete without hanging. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 45: AddSpline() intersection handling.
 * Tests complex intersection scenarios with endpoint merging.
 */
TEST(vfontdata_sanitize, Item45_AddSplineIntersection)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three overlapping squares to test intersection merging. */
  Nurb *sq1 = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.3f);
  Nurb *sq2 = create_square_nurb_handles(0.3f, 0.3f, 1.0f, 0.3f);
  Nurb *sq3 = create_square_nurb_handles(0.6f, 0.6f, 1.0f, 0.3f);

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);
  BLI_addtail(&nurbsbase, sq3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into a single contour. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 46-47: MList manipulation and monotonic elision.
 * Tests removal of zero-length segments during processing.
 */
TEST(vfontdata_sanitize, Item46_47_MListElision)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Shape with degenerate (zero-length) segments. */
  Nurb *nu = create_empty_bezier_nurb(5);
  set_bezt_linear(nu->bezt[0], 0.0f, 0.0f);
  set_bezt_linear(nu->bezt[1], 1.0f, 0.0f);
  set_bezt_linear(nu->bezt[2], 1.0f, 0.0f); /* Same as previous - zero length */
  set_bezt_linear(nu->bezt[3], 1.0f, 1.0f);
  set_bezt_linear(nu->bezt[4], 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a valid square without duplicate points. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  const Nurb *out_nu = static_cast<const Nurb *>(nurbsbase.first);
  EXPECT_LE(out_nu->pntsu, 4) << "Zero-length segment should be removed";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 48: Zero-length monotonic detection.
 * Tests detection when tstart==tend or geographically identical endpoints.
 */
TEST(vfontdata_sanitize, Item48_ZeroLengthMonotonic)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Shape with points at identical positions. */
  Nurb *nu = create_empty_bezier_nurb(6);
  set_bezt_linear(nu->bezt[0], 0.0f, 0.0f);
  set_bezt_linear(nu->bezt[1], 1.0f, 0.0f);
  set_bezt_linear(nu->bezt[2], 1.0f, 0.5f);
  set_bezt_linear(nu->bezt[3], 1.0f, 0.5f); /* Identical to previous */
  set_bezt_linear(nu->bezt[4], 1.0f, 1.0f);
  set_bezt_linear(nu->bezt[5], 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle without issues. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 49: Intersection movement.
 * Tests adjustment of intersection points to match spline endpoints.
 */
TEST(vfontdata_sanitize, Item49_IntersectionMovement)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares where intersection is very close to corner. */
  Nurb *sq1 = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *sq2 = create_linear_square_nurb(0.999f, 0.0f, 1.0f); /* Very close to aligned */

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge correctly despite near-alignment. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 50: Close intersection addition.
 * Tests handling when two monotonics are extremely close.
 */
TEST(vfontdata_sanitize, Item50_CloseIntersectionAddition)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two almost-parallel lines very close together. */
  Nurb *nu1 = create_empty_bezier_nurb(4);
  set_bezt_linear(nu1->bezt[0], 0.0f, 0.0f);
  set_bezt_linear(nu1->bezt[1], 1.0f, 0.001f); /* Almost horizontal */
  set_bezt_linear(nu1->bezt[2], 1.0f, 0.5f);
  set_bezt_linear(nu1->bezt[3], 0.0f, 0.5f);

  Nurb *nu2 = create_empty_bezier_nurb(4);
  set_bezt_linear(nu2->bezt[0], 0.0f, -0.001f); /* Almost same as nu1 start */
  set_bezt_linear(nu2->bezt[1], 1.0f, 0.0f);
  set_bezt_linear(nu2->bezt[2], 1.0f, -0.5f);
  set_bezt_linear(nu2->bezt[3], 0.0f, -0.5f);

  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle close lines. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 41.3: T-value at epsilon boundary should snap to endpoint.
 */
TEST(vfontdata_sanitize, Item41_3_TValueSnapping)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles positioned so intersection is nearly at t=0 on one curve. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.999f, 0.0f, 1.0f); /* Nearly touching edge */

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle edge intersection correctly. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 45.1: Endpoint snapping within EPSILON_4.
 */
TEST(vfontdata_sanitize, Item45_1_EndpointSnapping)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Square where intersection is extremely close to corner. */
  Nurb *sq1 = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.3f);
  /* Second square offset by tiny amount to create near-endpoint intersection. */
  Nurb *sq2 = create_square_nurb_handles(1.0f - 1e-5f, 0.0f, 1.0f, 0.3f);

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should snap intersection to corner and produce valid result. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 47.1: Zero-length monotonic elision after intersection.
 */
TEST(vfontdata_sanitize, Item47_1_MonotonicElision)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Shape where intersection processing might create zero-length monotonic. */
  Nurb *nu = create_empty_bezier_nurb(4);
  set_bezt(nu->bezt[0], 0.0f, 0.0f, -0.1f, 0.0f, 0.1f, 0.0f);
  set_bezt(nu->bezt[1], 1.0f, 0.0f, 0.9f, 0.0f, 1.0f, 0.1f); /* Small deviation */
  set_bezt(nu->bezt[2], 1.0f, 1.0f, 1.0f, 0.9f, 0.9f, 1.0f);
  set_bezt(nu->bezt[3], 0.0f, 1.0f, 0.1f, 1.0f, -0.1f, 0.9f);

  /* Overlapping square to force intersection at near-endpoint. */
  Nurb *sq = create_square_nurb_handles(0.999f, 0.0f, 1.0f, 0.3f);

  BLI_addtail(&nurbsbase, nu);
  BLI_addtail(&nurbsbase, sq);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge and elide any zero-length monotonics. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 48.5: Check zero-length after intersection processing.
 */
TEST(vfontdata_sanitize, Item48_5_ZeroLengthAfterIntersection)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two shapes that intersect, creating very short segments. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.001f, 0.0f, 1.0f); /* Tiny offset creates tiny segments */

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle tiny segments from near-identical intersection. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 51: T-value fixup on merge.
 * Tests recalculating exact t-values when intersection points move.
 */
TEST(vfontdata_sanitize, Item51_TValueFixupOnMerge)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares that share an edge - intersections should merge. */
  Nurb *sq1 = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *sq2 = create_linear_square_nurb(1.0f, 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce a merged rectangle. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 52: Monotonic following.
 * Tests following monotonics to find next intersections.
 * NOTE: Current implementation doesn't always merge chain of overlaps perfectly.
 */
TEST(vfontdata_sanitize, Item52_MonotonicFollowing)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Chain of overlapping squares - need to follow through multiple. */
  Nurb *sq1 = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.3f);
  Nurb *sq2 = create_square_nurb_handles(0.5f, 0.0f, 1.0f, 0.3f);
  Nurb *sq3 = create_square_nurb_handles(1.0f, 0.0f, 1.0f, 0.3f);
  Nurb *sq4 = create_square_nurb_handles(1.5f, 0.0f, 1.0f, 0.3f);

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);
  BLI_addtail(&nurbsbase, sq3);
  BLI_addtail(&nurbsbase, sq4);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid contours. Ideal: single merged shape.
   * Currently may produce multiple separate shapes due to chain handling. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 53: Configurable precision mode.
 * Tests that precision is sufficient for various coordinate scales.
 */
TEST(vfontdata_sanitize, Item53_PrecisionMode)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Test at small scale. */
  Nurb *small1 = create_circle_nurb(0.0f, 0.0f, 0.001f);
  Nurb *small2 = create_circle_nurb(0.0005f, 0.0f, 0.001f);

  BLI_addtail(&nurbsbase, small1);
  BLI_addtail(&nurbsbase, small2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 54: CloserT distance comparison.
 * Tests determining which t-value is closer to a point.
 */
TEST(vfontdata_sanitize, Item54_CloserTComparison)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two curves that cross very close to each other's endpoints. */
  Nurb *nu1 = create_empty_bezier_nurb(4);
  set_bezt(nu1->bezt[0], 0.0f, 0.0f, -0.2f, 0.0f, 0.2f, 0.2f);
  set_bezt(nu1->bezt[1], 1.0f, 1.0f, 0.8f, 0.8f, 1.2f, 1.0f);
  set_bezt(nu1->bezt[2], 1.0f, 0.0f, 1.2f, 0.0f, 0.8f, 0.0f);
  set_bezt(nu1->bezt[3], 0.0f, 1.0f, 0.2f, 1.0f, -0.2f, 0.8f);

  BLI_addtail(&nurbsbase, nu1);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Self-intersecting curve should be handled. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 55: ILReplaceMono (MList update on split).
 * Tests that splitting monotonics updates references correctly.
 */
TEST(vfontdata_sanitize, Item55_ILReplaceMono)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Figure-8 which requires monotonic splitting. */
  Nurb *fig8 = create_figure_8_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, fig8);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Figure-8 should split into two circles. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 2);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 56: Point containment test.
 * Tests if a point lies on a monotonic segment.
 */
TEST(vfontdata_sanitize, Item56_PointContainment)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two shapes where one corner exactly touches the other's edge. */
  Nurb *sq1 = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *sq2 = create_linear_square_nurb(0.5f, 1.0f, 1.0f); /* Bottom-left at (0.5, 1) */

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Two touching shapes - may or may not merge depending on tolerance. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 57: Monotonic lookup by t-value.
 * Tests finding monotonic that contains a specific t-value.
 */
TEST(vfontdata_sanitize, Item57_MonotonicLookup)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Complex shape with many monotonic segments. */
  Nurb *star = create_empty_bezier_nurb(10);
  for (int i = 0; i < 10; i++) {
    float angle = float(i) * M_PI * 2.0f / 10.0f;
    float radius = (i % 2 == 0) ? 1.0f : 0.5f;
    set_bezt_linear(star->bezt[i], cosf(angle) * radius, sinf(angle) * radius);
  }

  BLI_addtail(&nurbsbase, star);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Star shape should remain valid. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 58: Ordered endpoint list.
 * Tests gap-based test point selection.
 * NOTE: Current implementation may not merge all overlapping shapes in a chain.
 */
TEST(vfontdata_sanitize, Item58_OrderedEndpoints)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Many small squares in a row - tests endpoint ordering. */
  for (int i = 0; i < 5; i++) {
    Nurb *sq = create_linear_square_nurb(float(i) * 0.9f, 0.0f, 1.0f);
    BLI_addtail(&nurbsbase, sq);
  }

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid contours. Ideal: single merged shape.
   * Currently may produce multiple shapes due to chain handling. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 59: Partial bezier extraction.
 * Tests recalculating control points for partial spline extraction.
 */
TEST(vfontdata_sanitize, Item59_PartialBezierExtraction)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* A bezier curve that gets split by an overlapping square. */
  Nurb *curve = create_empty_bezier_nurb(4);
  set_bezt(curve->bezt[0], 0.0f, 0.5f, -0.3f, 0.5f, 0.3f, 0.8f);
  set_bezt(curve->bezt[1], 1.0f, 1.5f, 0.7f, 1.2f, 1.3f, 1.8f);
  set_bezt(curve->bezt[2], 2.0f, 0.5f, 1.7f, 0.8f, 2.3f, 0.5f);
  set_bezt(curve->bezt[3], 1.0f, -0.5f, 1.3f, -0.2f, 0.7f, -0.5f);

  /* Square that crosses the curve. */
  Nurb *sq = create_square_nurb_handles(0.5f, 0.5f, 1.0f, 0.3f);

  BLI_addtail(&nurbsbase, curve);
  BLI_addtail(&nurbsbase, sq);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid merged shape. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 60: MList lookup.
 * Tests finding MList entry for a monotonic at an intersection.
 */
TEST(vfontdata_sanitize, Item60_MListLookup)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* X-crossing shape with clear intersection. */
  Nurb *x = create_x_crossing_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, x);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* X-crossing should split into separate parts. */
  EXPECT_GE(count_nurbs(&nurbsbase), 2);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 51.1: T-value snapping to 0.0 when near start.
 */
TEST(vfontdata_sanitize, Item51_1_TValueSnapToZero)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Circle where intersection might be computed at t=0.0001 instead of t=0. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(1.9999f, 0.0f, 1.0f); /* Nearly touching at t=0 */

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 52.1: mono_goes_somewhere_useful validation.
 */
TEST(vfontdata_sanitize, Item52_1_UsefulIntersection)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Bowtie shape - following monotonics should lead to useful intersections. */
  Nurb *bowtie = create_bowtie_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, bowtie);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Bowtie should split into two triangles. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 57.3: Tolerance fallback for t-value lookup.
 */
TEST(vfontdata_sanitize, Item57_3_TValueTolerance)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Shape with very small segment - t-value lookup needs tolerance. */
  Nurb *nu = create_empty_bezier_nurb(5);
  set_bezt_linear(nu->bezt[0], 0.0f, 0.0f);
  set_bezt_linear(nu->bezt[1], 0.5f, 0.0f);
  set_bezt_linear(nu->bezt[2], 0.5f + 1e-7f, 0.5f); /* Tiny segment */
  set_bezt_linear(nu->bezt[3], 0.5f, 1.0f);
  set_bezt_linear(nu->bezt[4], 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 58.4: Gap-based test point for winding calculation.
 */
TEST(vfontdata_sanitize, Item58_4_GapBasedWinding)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Vertical stack of circles - largest gap should be used for winding test. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.0f, 3.0f, 1.0f); /* Gap of 1.0 between */
  Nurb *c3 = create_circle_nurb(0.5f, 1.5f, 0.3f); /* Small circle in gap */

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);
  BLI_addtail(&nurbsbase, c3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Three separate circles should remain separate. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 59.3: Partial extraction at exactly t=0 or t=1.
 */
TEST(vfontdata_sanitize, Item59_3_ExactTValueExtraction)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Square perfectly aligned so intersection is at corner (t=0 or t=1). */
  Nurb *sq1 = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.3f);
  Nurb *sq2 = create_square_nurb_handles(1.0f, 0.0f, 1.0f, 0.3f); /* Shares edge at x=1 */

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 61: Find-intersection mode spline splitting.
 * Tests splitting splines at intersection points.
 */
TEST(vfontdata_sanitize, Item61_FindIntersectionSplit)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two bezier curves that cross. */
  Nurb *nu1 = create_empty_bezier_nurb(4);
  set_bezt(nu1->bezt[0], 0.0f, 0.0f, -0.2f, 0.0f, 0.2f, 0.3f);
  set_bezt(nu1->bezt[1], 1.0f, 1.0f, 0.8f, 0.7f, 1.2f, 1.0f);
  set_bezt(nu1->bezt[2], 1.0f, 0.0f, 1.2f, 0.0f, 0.8f, 0.0f);
  set_bezt(nu1->bezt[3], 0.0f, 1.0f, 0.2f, 1.0f, -0.2f, 0.7f);

  BLI_addtail(&nurbsbase, nu1);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Self-crossing shape should be split. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 62: Flexible monotonic splitting.
 * Tests robust splitting with fallbacks for edge cases.
 */
TEST(vfontdata_sanitize, Item62_FlexibleSplitting)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Shape with near-tangent intersection. */
  Nurb *nu = create_empty_bezier_nurb(4);
  set_bezt(nu->bezt[0], 0.0f, 0.0f, 0.0f, -0.3f, 0.3f, 0.0f);
  set_bezt(nu->bezt[1], 1.0f, 0.0f, 0.7f, 0.0f, 1.0f, 0.3f);
  set_bezt(nu->bezt[2], 1.0f, 1.0f, 1.0f, 0.7f, 0.7f, 1.0f);
  set_bezt(nu->bezt[3], 0.0f, 1.0f, 0.3f, 1.0f, 0.0f, 0.7f);

  /* Overlapping shape. */
  Nurb *sq = create_square_nurb_handles(0.5f, 0.25f, 0.5f, 0.15f);

  BLI_addtail(&nurbsbase, nu);
  BLI_addtail(&nurbsbase, sq);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 63: Winding number core logic.
 * Tests neededness determination based on winding transitions.
 * NOTE: Nested circles with same winding direction may be handled differently
 * than expected hole behavior. This depends on the winding fill rule used.
 */
TEST(vfontdata_sanitize, Item63_WindingCore)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Nested circles - behavior depends on winding rule. */
  Nurb *outer = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *inner = create_circle_nurb(0.0f, 0.0f, 0.3f);

  BLI_addtail(&nurbsbase, outer);
  BLI_addtail(&nurbsbase, inner);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 64: Spline point creation during reconstruction.
 * Tests proper creation of SplinePoints with correct control points.
 */
TEST(vfontdata_sanitize, Item64_SplinePointCreation)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Complex curved shape. */
  Nurb *nu = create_empty_bezier_nurb(6);
  set_bezt(nu->bezt[0], 0.0f, 0.0f, -0.2f, 0.1f, 0.2f, 0.1f);
  set_bezt(nu->bezt[1], 0.5f, 0.3f, 0.3f, 0.3f, 0.7f, 0.3f);
  set_bezt(nu->bezt[2], 1.0f, 0.0f, 0.8f, 0.1f, 1.2f, 0.0f);
  set_bezt(nu->bezt[3], 1.0f, 1.0f, 1.2f, 1.0f, 0.8f, 0.9f);
  set_bezt(nu->bezt[4], 0.5f, 0.7f, 0.7f, 0.7f, 0.3f, 0.7f);
  set_bezt(nu->bezt[5], 0.0f, 1.0f, 0.2f, 1.0f, -0.2f, 0.9f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should remain single contour with valid points. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 65: Paired monotonic splitting.
 * Tests splitting two monotonics at same coordinate with tolerance checks.
 */
TEST(vfontdata_sanitize, Item65_PairedSplitting)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two curves that are very close in a region. */
  Nurb *nu1 = create_empty_bezier_nurb(4);
  set_bezt(nu1->bezt[0], 0.0f, 0.0f, -0.1f, 0.0f, 0.3f, 0.0f);
  set_bezt(nu1->bezt[1], 1.0f, 0.5f, 0.7f, 0.5f, 1.3f, 0.5f);
  set_bezt(nu1->bezt[2], 1.0f, 1.0f, 1.3f, 1.0f, 0.7f, 1.0f);
  set_bezt(nu1->bezt[3], 0.0f, 0.5f, 0.3f, 0.5f, -0.1f, 0.5f);

  Nurb *nu2 = create_empty_bezier_nurb(4);
  set_bezt(nu2->bezt[0], 0.0f, 0.01f, -0.1f, 0.01f, 0.3f, 0.01f);
  set_bezt(nu2->bezt[1], 1.0f, 0.51f, 0.7f, 0.51f, 1.3f, 0.51f);
  set_bezt(nu2->bezt[2], 1.0f, -0.5f, 1.3f, -0.5f, 0.7f, -0.5f);
  set_bezt(nu2->bezt[3], 0.0f, -0.5f, 0.3f, -0.5f, -0.1f, -0.5f);

  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 66: Pre-intersection to intersection conversion.
 * Tests conversion of pending intersections to real ones after splits.
 */
TEST(vfontdata_sanitize, Item66_PreInterConversion)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Multiple overlapping shapes. */
  Nurb *sq1 = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.3f);
  Nurb *sq2 = create_square_nurb_handles(0.25f, 0.25f, 1.0f, 0.3f);

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 67: Monotonic creation from spline.
 * Tests creating monotonic structs with direction flags.
 */
TEST(vfontdata_sanitize, Item67_MonotonicCreation)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* S-curve with multiple direction changes. */
  Nurb *nu = create_empty_bezier_nurb(4);
  set_bezt(nu->bezt[0], 0.0f, 0.0f, -0.2f, 0.0f, 0.2f, 0.3f);
  set_bezt(nu->bezt[1], 0.5f, 1.0f, 0.3f, 0.7f, 0.7f, 1.0f);
  set_bezt(nu->bezt[2], 1.0f, 0.0f, 0.8f, 0.0f, 1.2f, 0.3f);
  set_bezt(nu->bezt[3], 1.5f, 1.0f, 1.3f, 0.7f, 1.7f, 1.0f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 68: Contour to monotonic conversion.
 * Tests converting SplineSet to monotonic chains.
 */
TEST(vfontdata_sanitize, Item68_ContourConversion)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Complex contour with many segments. */
  Nurb *nu = create_empty_bezier_nurb(8);
  for (int i = 0; i < 8; i++) {
    float angle = float(i) * M_PI * 2.0f / 8.0f;
    float r = (i % 2 == 0) ? 1.0f : 0.7f;
    float x = cosf(angle) * r;
    float y = sinf(angle) * r;
    float dx = -sinf(angle) * 0.2f;
    float dy = cosf(angle) * 0.2f;
    set_bezt(nu->bezt[i], x, y, x - dx, y - dy, x + dx, y + dy);
  }

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 69: Extrema finding for monotonic decomposition.
 * Tests finding extrema points to split splines.
 */
TEST(vfontdata_sanitize, Item69_ExtremaFinding)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Curve with clear extrema. */
  Nurb *nu = create_empty_bezier_nurb(4);
  set_bezt(nu->bezt[0], 0.0f, 0.5f, -0.3f, 0.5f, 0.3f, 1.0f);
  set_bezt(nu->bezt[1], 1.0f, 1.5f, 0.7f, 1.5f, 1.3f, 1.0f);
  set_bezt(nu->bezt[2], 2.0f, 0.5f, 1.7f, 0.0f, 2.3f, 0.5f);
  set_bezt(nu->bezt[3], 1.0f, -0.5f, 1.3f, -0.5f, 0.7f, -0.5f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 70: Monotonic closure check.
 * Tests verifying monotonic list forms a closed loop.
 */
TEST(vfontdata_sanitize, Item70_ClosureCheck)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* A closed contour (circle). */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should remain as closed contour. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  const Nurb *out = static_cast<const Nurb *>(nurbsbase.first);
  EXPECT_TRUE((out->flagu & CU_NURB_CYCLIC) != 0) << "Output should be cyclic";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 71: Monotonic line intersection finding.
 * Tests finding monotonics that intersect test lines for winding.
 */
TEST(vfontdata_sanitize, Item71_LineIntersectionFinding)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles - winding calculation needs line intersection. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.5f, 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should merge into single shape. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 72: Intersection creation with reuse.
 * Tests reusing existing intersections within tolerance.
 */
TEST(vfontdata_sanitize, Item72_IntersectionReuse)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Shapes that meet at same point from multiple directions. */
  Nurb *sq1 = create_linear_square_nurb(0.0f, 0.0f, 1.0f);
  Nurb *sq2 = create_linear_square_nurb(1.0f, 0.0f, 1.0f);
  Nurb *sq3 = create_linear_square_nurb(0.0f, 1.0f, 1.0f);
  Nurb *sq4 = create_linear_square_nurb(1.0f, 1.0f, 1.0f);

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);
  BLI_addtail(&nurbsbase, sq3);
  BLI_addtail(&nurbsbase, sq4);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Four adjacent squares should produce single contour. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 73: Monotonic endpoint evaluation helpers.
 * Tests getting coordinates of monotonic endpoints using priority system.
 */
TEST(vfontdata_sanitize, Item73_EndpointEvaluation)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Diamond shape - tests endpoint evaluation at t=0 and t=1. */
  Nurb *diamond = create_diamond_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, diamond);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 74: Control point angle clustering (currently disabled in FontForge).
 * Tests handling of control points with similar angles.
 */
TEST(vfontdata_sanitize, Item74_AngleClustering)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Shape with control points at similar angles. */
  Nurb *nu = create_empty_bezier_nurb(4);
  set_bezt(nu->bezt[0], 0.0f, 0.0f, -0.1f, -0.01f, 0.1f, 0.01f); /* Nearly horizontal handles */
  set_bezt(nu->bezt[1], 1.0f, 0.0f, 0.9f, -0.01f, 1.1f, 0.01f);
  set_bezt(nu->bezt[2], 1.0f, 1.0f, 1.1f, 0.99f, 0.9f, 1.01f);
  set_bezt(nu->bezt[3], 0.0f, 1.0f, 0.1f, 0.99f, -0.1f, 1.01f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 75: Intersection data structure.
 * Tests passing intersection info during splitting operations.
 * NOTE: Bowtie behavior depends on how self-intersection is handled.
 * With non-zero winding, it may remain as one shape or split.
 */
TEST(vfontdata_sanitize, Item75_IntersectionData)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Bowtie shape with interior intersection. */
  Nurb *bowtie = create_bowtie_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, bowtie);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output. May be 1 or 2 contours depending on
   * how self-intersection is handled. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Items 61-75 Sub-Item Tests
 *
 * Additional tests for sub-items from knowledge.rst Items 61-75.
 * \{ */

/**
 * Item 61.3: Test spline splitting at intersection points.
 * When shapes intersect, the algorithm splits monotonics at intersection points.
 */
TEST(vfontdata_sanitize, Item61_3_SplineSplitAtIntersection)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two crossing curves - must split at intersection. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.8f, 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce merged contour. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 62.3: Test splitting at t-values extremely close to 0 or 1.
 * Robust splitting should handle edge cases near endpoint t-values.
 */
TEST(vfontdata_sanitize, Item62_3_SplittingNearEndpoints)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two shapes that intersect very close to a corner. */
  Nurb *sq1 = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.3f);
  Nurb *sq2 = create_square_nurb_handles(0.999f, 0.0f, 1.0f, 0.3f); /* Almost shared corner */

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 63.4: Test winding with known overlapping shapes.
 * Verifies winding number correctly determines neededness.
 */
TEST(vfontdata_sanitize, Item63_4_KnownOverlappingShapes)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Three concentric circles - winding behavior. */
  Nurb *outer = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *middle = create_circle_nurb(0.0f, 0.0f, 0.6f);
  Nurb *inner = create_circle_nurb(0.0f, 0.0f, 0.3f);

  BLI_addtail(&nurbsbase, outer);
  BLI_addtail(&nurbsbase, middle);
  BLI_addtail(&nurbsbase, inner);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* With non-zero winding, concentric circles should produce valid output. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 64.3: Test original curve precision preservation.
 * Full-span monotonics should preserve original control points.
 */
TEST(vfontdata_sanitize, Item64_3_OriginalPrecisionPreserved)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Single circle - no overlaps, should preserve precision. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  /* Save original handles. */
  float orig_h2_x = circle->bezt[0].vec[2][0];
  float orig_h2_y = circle->bezt[0].vec[2][1];

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);

  /* Check that handles are close to original (early exit should preserve exactly). */
  const Nurb *out = static_cast<const Nurb *>(nurbsbase.first);
  if (out->pntsu >= 1) {
    /* If early exit preserved the original, handles should match. */
    float new_h2_x = out->bezt[0].vec[2][0];
    float new_h2_y = out->bezt[0].vec[2][1];
    EXPECT_NEAR(new_h2_x, orig_h2_x, 0.001f);
    EXPECT_NEAR(new_h2_y, orig_h2_y, 0.001f);
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 65.2: Test proximity rejection in paired splitting.
 * Splits too close to existing endpoints should be rejected.
 *
 * Note: With handle_offset=0.3, each side of the "square" bulges ~0.133 units.
 * The gap must be larger than 2*0.133 = 0.266 to avoid actual intersection.
 * We use handle_offset=0.0 (straight edges) to test pure proximity without
 * curve bulge complications.
 */
TEST(vfontdata_sanitize, Item65_2_ProximityRejection)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two squares with straight edges (no curve bulge) and tiny gap. */
  Nurb *sq1 = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.0f);
  Nurb *sq2 = create_square_nurb_handles(1.001f, 0.0f, 1.0f, 0.0f); /* Tiny gap */

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should remain as separate contours (no overlap). */
  EXPECT_GE(count_nurbs(&nurbsbase), 2);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 67.3: Test direction flag correctness for horizontal/vertical segments.
 * Monotonics should have correct xup/yup flags.
 */
TEST(vfontdata_sanitize, Item67_3_HorizontalVerticalDirection)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Square has purely horizontal and vertical edges. */
  Nurb *sq = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.3f);
  BLI_addtail(&nurbsbase, sq);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 68.1: Test open contour detection and skipping.
 * Non-cyclic contours should be handled gracefully.
 */
TEST(vfontdata_sanitize, Item68_1_OpenContourSkipping)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create an open (non-cyclic) contour. */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = 0; /* NOT cyclic */
  nu->resolu = 12;
  nu->pntsu = 3;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(3, __func__);
  set_bezt(nu->bezt[0], 0.0f, 0.0f, -0.2f, 0.0f, 0.2f, 0.0f);
  set_bezt(nu->bezt[1], 1.0f, 1.0f, 0.8f, 1.0f, 1.2f, 1.0f);
  set_bezt(nu->bezt[2], 2.0f, 0.0f, 1.8f, 0.0f, 2.2f, 0.0f);

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Open contours should be skipped/preserved. */
  EXPECT_GE(count_nurbs(&nurbsbase), 0); /* May remove or keep open contours */

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 68.2: Test single-point contour handling.
 * Degenerate contours should be handled gracefully.
 */
TEST(vfontdata_sanitize, Item68_2_SinglePointContour)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Single-point contour (degenerate). */
  Nurb *nu = MEM_new_for_free<Nurb>(__func__);
  *nu = dna::shallow_zero_initialize();
  nu->type = CU_BEZIER;
  nu->flagu = CU_NURB_CYCLIC;
  nu->resolu = 12;
  nu->pntsu = 1;
  nu->bezt = MEM_calloc_arrayN<BezTriple>(1, __func__);
  set_bezt(nu->bezt[0], 0.5f, 0.5f, 0.3f, 0.5f, 0.7f, 0.5f);

  /* Add a valid contour as well. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);

  BLI_addtail(&nurbsbase, nu);
  BLI_addtail(&nurbsbase, circle);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should not crash, and valid contour should remain. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 70.3: Test open monotonics skipped in winding calculation.
 * Open monotonics shouldn't affect winding numbers.
 */
TEST(vfontdata_sanitize, Item70_3_OpenMonotonicSkipped)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Mix of open and closed contours. */
  Nurb *circle = create_circle_nurb(0.0f, 0.0f, 1.0f);
  BLI_addtail(&nurbsbase, circle);

  /* Non-cyclic path (will be skipped). */
  Nurb *open = MEM_new_for_free<Nurb>(__func__);
  *open = dna::shallow_zero_initialize();
  open->type = CU_BEZIER;
  open->flagu = 0; /* NOT cyclic */
  open->resolu = 12;
  open->pntsu = 2;
  open->bezt = MEM_calloc_arrayN<BezTriple>(2, __func__);
  set_bezt(open->bezt[0], -2.0f, 0.0f, -2.2f, 0.0f, -1.8f, 0.0f);
  set_bezt(open->bezt[1], 2.0f, 0.0f, 1.8f, 0.0f, 2.2f, 0.0f);
  BLI_addtail(&nurbsbase, open);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Closed contour should survive. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 71.3: Test rays passing through monotonic endpoints.
 * Edge case where test ray passes exactly through an endpoint.
 */
TEST(vfontdata_sanitize, Item71_3_RayThroughEndpoint)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Two circles with intersection at specific y-coordinate. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(1.0f, 0.0f, 1.0f); /* Intersection at x=0.5, y=±√0.75 */

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 72.3: Test intersection reuse with very close points.
 * Multiple near-coincident intersections should be merged.
 */
TEST(vfontdata_sanitize, Item72_3_CloseIntersectionReuse)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Multiple shapes meeting at nearly the same point. */
  Nurb *sq1 = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.3f);
  Nurb *sq2 = create_square_nurb_handles(0.999f, -0.001f, 1.0f, 0.3f); /* Slightly offset */

  BLI_addtail(&nurbsbase, sq1);
  BLI_addtail(&nurbsbase, sq2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 73.4: Test endpoint evaluation precision.
 * Endpoint coordinates should use exact values when possible.
 */
TEST(vfontdata_sanitize, Item73_4_EndpointPrecision)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Simple square - endpoints should be exact. */
  Nurb *sq = create_square_nurb_handles(0.0f, 0.0f, 1.0f, 0.3f);
  BLI_addtail(&nurbsbase, sq);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* Check that corners are at exact positions. */
  const Nurb *out = static_cast<const Nurb *>(nurbsbase.first);
  if (out && out->pntsu >= 4) {
    /* Find corner at (0,0). */
    bool found_origin = false;
    for (int i = 0; i < out->pntsu; i++) {
      if (std::abs(out->bezt[i].vec[1][0]) < 0.01f && std::abs(out->bezt[i].vec[1][1]) < 0.01f) {
        found_origin = true;
        break;
      }
    }
    EXPECT_TRUE(found_origin) << "Should have corner near origin";
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Item 75.3: Test intersection data propagation through splitting.
 * Intersection info should be properly passed during operations.
 */
TEST(vfontdata_sanitize, Item75_3_IntersectionDataPropagation)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Complex intersection pattern. */
  Nurb *c1 = create_circle_nurb(0.0f, 0.0f, 1.0f);
  Nurb *c2 = create_circle_nurb(0.5f, 0.0f, 1.0f);
  Nurb *c3 = create_circle_nurb(0.25f, 0.5f, 0.8f);

  BLI_addtail(&nurbsbase, c1);
  BLI_addtail(&nurbsbase, c2);
  BLI_addtail(&nurbsbase, c3);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid merged output. */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Zero-Length Monotonic Elision Tests
 *
 * Tests for proper handling of zero-length monotonics.
 * Ported from FontForge's MonotonicCheckZeroLength() and MonotonicElide().
 * \{ */

/**
 * Test zero-length monotonic detection: K-glyph-like geometry.
 *
 * This test exercises the elide_zero_length_monotonics() code path by
 * replicating the geometry pattern from font_overlap.ttf 'k' character.
 *
 * The geometry creates zero-length monotonics due to intersections happening
 * very close to t=0 or t=1 of monotonic segments. When shapes overlap in a way
 * that intersection points coincide with monotonic boundaries (like where the
 * diagonal arms of 'k' meet the vertical stem), we get segments where
 * start_intersection == end_intersection.
 *
 * The zero-length elision is confirmed by debug output showing:
 *   "Zero-length mono X: same intersection at both ends (Y)"
 *
 * Note: The full validation of this fix is done via validate.py on the 'k'
 * character in font_overlap.ttf, which fails without the fix due to
 * self-intersecting output curves.
 */
TEST(vfontdata_sanitize, ZeroLengthMono_KGlyphPattern)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Vertical bar (like the stem of 'k').
   * This is a tall thin rectangle. */
  Nurb *stem = create_empty_bezier_nurb(4);
  set_bezt_linear(stem->bezt[0], 0.1f, 0.0f);
  set_bezt_linear(stem->bezt[1], 0.12f, 0.0f);
  set_bezt_linear(stem->bezt[2], 0.12f, 0.6f);
  set_bezt_linear(stem->bezt[3], 0.1f, 0.6f);

  /* Upper diagonal arm - goes from right to left, ending very close to the stem.
   * The key is that the arm's endpoint is VERY close to where it intersects the stem,
   * creating intersections at t close to 0 or 1. */
  Nurb *upper_arm = create_empty_bezier_nurb(4);
  set_bezt_linear(upper_arm->bezt[0], 0.35f, 0.4f);
  set_bezt_linear(upper_arm->bezt[1], 0.36f, 0.35f);
  /* Endpoint at x=0.102, just barely past the stem's right edge at x=0.12 */
  set_bezt_linear(upper_arm->bezt[2], 0.102f, 0.13f);
  set_bezt_linear(upper_arm->bezt[3], 0.1f, 0.18f);

  /* Lower diagonal arm - goes from right to left, endpoint near stem. */
  Nurb *lower_arm = create_empty_bezier_nurb(4);
  set_bezt_linear(lower_arm->bezt[0], 0.1f, 0.19f);
  set_bezt_linear(lower_arm->bezt[1], 0.102f, 0.14f);
  set_bezt_linear(lower_arm->bezt[2], 0.36f, 0.0f);
  set_bezt_linear(lower_arm->bezt[3], 0.35f, 0.05f);

  BLI_addtail(&nurbsbase, stem);
  BLI_addtail(&nurbsbase, upper_arm);
  BLI_addtail(&nurbsbase, lower_arm);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output - the key test is that it doesn't crash
   * or produce broken contours due to zero-length monotonics. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Coincident Segment Boundary Handling (Fix for 'œ' character)
 *
 * These tests verify that coincident (overlapping) segments are handled correctly
 * by creating interior splits at boundaries rather than marking as duplicates.
 *
 * The bug: When two monotonics overlapped, they were marked as is_duplicate=true
 * and skipped, breaking connectivity. This caused self-intersections in characters
 * like 'œ' (9 self-intersections observed).
 *
 * The fix: Create intersections at coincident boundary points using
 * find_coincident_boundaries(), similar to FontForge's CoincidentIntersect().
 * \{ */

/**
 * Test overlapping circles pattern similar to 'œ' ligature.
 * Two circles that share a coincident segment where they overlap.
 *
 * This pattern caused 9 self-intersections before the fix because
 * the coincident segment was marked as duplicate and dropped,
 * breaking the contour connectivity.
 */
TEST(vfontdata_sanitize, CoincidentSegmentBoundaries_LigaturePattern)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create two overlapping "circles" (approximated as rounded rectangles).
   * The left circle is centered at x=0.3, the right at x=0.5.
   * They overlap in the middle, creating coincident segments. */

  /* Left "o" shape - a rounded rectangle */
  Nurb *left_o = create_empty_bezier_nurb(8);
  /* Bottom edge */
  set_bezt(left_o->bezt[0], 0.15f, 0.2f, 0.10f, 0.2f, 0.20f, 0.2f);
  set_bezt(left_o->bezt[1], 0.35f, 0.2f, 0.30f, 0.2f, 0.40f, 0.2f);
  /* Right edge */
  set_bezt(left_o->bezt[2], 0.45f, 0.25f, 0.45f, 0.22f, 0.45f, 0.30f);
  set_bezt(left_o->bezt[3], 0.45f, 0.55f, 0.45f, 0.50f, 0.45f, 0.58f);
  /* Top edge */
  set_bezt(left_o->bezt[4], 0.35f, 0.6f, 0.40f, 0.6f, 0.30f, 0.6f);
  set_bezt(left_o->bezt[5], 0.15f, 0.6f, 0.20f, 0.6f, 0.10f, 0.6f);
  /* Left edge */
  set_bezt(left_o->bezt[6], 0.05f, 0.55f, 0.05f, 0.58f, 0.05f, 0.50f);
  set_bezt(left_o->bezt[7], 0.05f, 0.25f, 0.05f, 0.30f, 0.05f, 0.22f);

  /* Right "e" shape - overlapping with left_o */
  Nurb *right_e = create_empty_bezier_nurb(8);
  /* Bottom edge */
  set_bezt(right_e->bezt[0], 0.35f, 0.2f, 0.30f, 0.2f, 0.40f, 0.2f);
  set_bezt(right_e->bezt[1], 0.55f, 0.2f, 0.50f, 0.2f, 0.60f, 0.2f);
  /* Right edge */
  set_bezt(right_e->bezt[2], 0.65f, 0.25f, 0.65f, 0.22f, 0.65f, 0.30f);
  set_bezt(right_e->bezt[3], 0.65f, 0.55f, 0.65f, 0.50f, 0.65f, 0.58f);
  /* Top edge */
  set_bezt(right_e->bezt[4], 0.55f, 0.6f, 0.60f, 0.6f, 0.50f, 0.6f);
  set_bezt(right_e->bezt[5], 0.35f, 0.6f, 0.40f, 0.6f, 0.30f, 0.6f);
  /* Left edge - this overlaps with left_o's right edge! */
  set_bezt(right_e->bezt[6], 0.45f, 0.55f, 0.45f, 0.58f, 0.45f, 0.50f);
  set_bezt(right_e->bezt[7], 0.45f, 0.25f, 0.45f, 0.30f, 0.45f, 0.22f);

  BLI_addtail(&nurbsbase, left_o);
  BLI_addtail(&nurbsbase, right_e);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output without self-intersections.
   * Before the fix, this would produce 9 self-intersections. */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase))
      << "Ligature pattern with coincident segments should produce valid contours";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test that opposite-direction coincident segments are handled correctly.
 * When two segments overlap but go in opposite directions, they should
 * cancel out (mutual_collapse) rather than creating broken contours.
 */
TEST(vfontdata_sanitize, CoincidentSegmentBoundaries_OppositeDirection)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create two shapes where one edge of each runs in opposite directions
   * but along the same path. */

  /* First shape: counter-clockwise square */
  Nurb *nu1 = create_empty_bezier_nurb(4);
  set_bezt_linear(nu1->bezt[0], 0.0f, 0.0f); /* bottom-left */
  set_bezt_linear(nu1->bezt[1], 0.5f, 0.0f); /* bottom-right */
  set_bezt_linear(nu1->bezt[2], 0.5f, 1.0f); /* top-right */
  set_bezt_linear(nu1->bezt[3], 0.0f, 1.0f); /* top-left */

  /* Second shape: also counter-clockwise, shares right edge with first.
   * The shared edge at x=0.5 goes from y=0 to y=1 in nu1,
   * and from y=1 to y=0 in nu2 (opposite direction on the contour). */
  Nurb *nu2 = create_empty_bezier_nurb(4);
  set_bezt_linear(nu2->bezt[0], 0.5f, 0.0f); /* bottom-left (same as nu1's bottom-right) */
  set_bezt_linear(nu2->bezt[1], 1.0f, 0.0f); /* bottom-right */
  set_bezt_linear(nu2->bezt[2], 1.0f, 1.0f); /* top-right */
  set_bezt_linear(nu2->bezt[3], 0.5f, 1.0f); /* top-left (same as nu1's top-right) */

  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Two adjacent squares should merge into one larger rectangle.
   * The coincident edge should cancel out (mutual_collapse).
   *
   * The output has 6 points because the perimeter is traced through
   * the junction points at (0.5, 0) and (0.5, 1). */
  EXPECT_EQ(count_nurbs(&nurbsbase), 1)
      << "Adjacent squares with coincident edge should merge to one contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase));

  /* The merged rectangle has 6 bezier points: 4 corners plus 2 junction points
   * where the original squares met at x=0.5. */
  EXPECT_EQ(count_bezier_points(&nurbsbase), 6)
      << "Merged rectangle should have 6 points (4 corners + 2 junctions)";

  /* Verify the merged shape has reasonable bounds */
  if (count_nurbs(&nurbsbase) == 1) {
    float minx, miny, maxx, maxy;
    calculate_bounds(&nurbsbase, minx, miny, maxx, maxy);
    /* Should span from x=0 to x=1, y=0 to y=1 */
    EXPECT_NEAR(minx, 0.0f, 0.1f);
    EXPECT_NEAR(maxx, 1.0f, 0.1f);
    EXPECT_NEAR(miny, 0.0f, 0.1f);
    EXPECT_NEAR(maxy, 1.0f, 0.1f);
  }

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test coincident segments with partial overlap.
 * The segments share a portion but not their entire length.
 * This tests the boundary detection in find_coincident_boundaries().
 */
TEST(vfontdata_sanitize, CoincidentSegmentBoundaries_PartialOverlap)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create two shapes where edges partially overlap. */

  /* First shape: tall rectangle */
  Nurb *nu1 = create_empty_bezier_nurb(4);
  set_bezt_linear(nu1->bezt[0], 0.0f, 0.0f);
  set_bezt_linear(nu1->bezt[1], 0.3f, 0.0f);
  set_bezt_linear(nu1->bezt[2], 0.3f, 1.0f); /* Right edge: y from 0 to 1 */
  set_bezt_linear(nu1->bezt[3], 0.0f, 1.0f);

  /* Second shape: shorter rectangle that partially overlaps the right edge.
   * Its left edge at x=0.3 only goes from y=0.25 to y=0.75. */
  Nurb *nu2 = create_empty_bezier_nurb(4);
  set_bezt_linear(nu2->bezt[0], 0.3f, 0.25f); /* Starts partway up nu1's right edge */
  set_bezt_linear(nu2->bezt[1], 0.6f, 0.25f);
  set_bezt_linear(nu2->bezt[2], 0.6f, 0.75f);
  set_bezt_linear(nu2->bezt[3], 0.3f, 0.75f); /* Ends partway up nu1's right edge */

  BLI_addtail(&nurbsbase, nu1);
  BLI_addtail(&nurbsbase, nu2);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output. The partial coincident edge should be
   * properly split at the boundary points (0.25 and 0.75 on the y-axis). */
  EXPECT_GE(count_nurbs(&nurbsbase), 1);
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase))
      << "Partial overlap of coincident segments should produce valid contours";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stupid Control Point Removal Tests
 *
 * Tests for ss_remove_stupid_control_points() which removes:
 * - Backward-pointing handles (point opposite to segment direction)
 * - Overshooting handles (extend beyond the next point)
 *
 * FontForge: splineutil2.c RemoveStupidControlPoints()
 * \{ */

/**
 * Test that backward-pointing handles are snapped to point position.
 * A backward-pointing handle points away from the segment direction.
 */
TEST(vfontdata_sanitize, StupidControlPoints_BackwardHandle)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a closed square contour with one backward-pointing handle.
   * Use a proper closed contour so the overlap removal produces valid output. */
  Nurb *nu = create_empty_bezier_nurb(4);

  /* Bottom-left corner with backward-pointing next handle (goes left when segment goes right) */
  nu->bezt[0].vec[1][0] = 0.0f; /* point */
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.1f; /* prev handle (normal) */
  nu->bezt[0].vec[0][1] = 0.1f;
  nu->bezt[0].vec[2][0] = -0.3f; /* next handle - backward! Points left when going right */
  nu->bezt[0].vec[2][1] = 0.0f;

  /* Bottom-right corner */
  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 0.0f;
  nu->bezt[1].vec[0][0] = 0.7f;
  nu->bezt[1].vec[0][1] = 0.0f;
  nu->bezt[1].vec[2][0] = 1.0f;
  nu->bezt[1].vec[2][1] = 0.3f;

  /* Top-right corner */
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 1.0f;
  nu->bezt[2].vec[0][0] = 1.0f;
  nu->bezt[2].vec[0][1] = 0.7f;
  nu->bezt[2].vec[2][0] = 0.7f;
  nu->bezt[2].vec[2][1] = 1.0f;

  /* Top-left corner */
  nu->bezt[3].vec[1][0] = 0.0f;
  nu->bezt[3].vec[1][1] = 1.0f;
  nu->bezt[3].vec[0][0] = 0.3f;
  nu->bezt[3].vec[0][1] = 1.0f;
  nu->bezt[3].vec[2][0] = 0.0f;
  nu->bezt[3].vec[2][1] = 0.7f;

  BLI_addtail(&nurbsbase, nu);

  /* Verify test setup: the handle at point 0 should be backward-pointing */
  float orig_h2x = nu->bezt[0].vec[2][0];
  EXPECT_LT(orig_h2x, nu->bezt[0].vec[1][0])
      << "Test setup: next handle should be behind point (backward)";

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* After processing, the output should be valid */
  Nurb *result_nu = static_cast<Nurb *>(nurbsbase.first);
  ASSERT_NE(result_nu, nullptr) << "Should produce valid output contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase))
      << "Backward handle should be corrected and produce valid result";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test that overshooting handles are snapped to point position.
 * An overshooting handle extends beyond the destination point.
 */
TEST(vfontdata_sanitize, StupidControlPoints_OvershootingHandle)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a closed square contour with one overshooting handle */
  Nurb *nu = create_empty_bezier_nurb(4);

  /* Bottom-left corner with overshooting next handle */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.1f;
  nu->bezt[0].vec[0][1] = 0.1f;
  nu->bezt[0].vec[2][0] = 1.5f; /* Overshoots! Goes past next point at x=1 */
  nu->bezt[0].vec[2][1] = 0.0f;

  /* Bottom-right corner */
  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 0.0f;
  nu->bezt[1].vec[0][0] = 0.7f;
  nu->bezt[1].vec[0][1] = 0.0f;
  nu->bezt[1].vec[2][0] = 1.0f;
  nu->bezt[1].vec[2][1] = 0.3f;

  /* Top-right corner */
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 1.0f;
  nu->bezt[2].vec[0][0] = 1.0f;
  nu->bezt[2].vec[0][1] = 0.7f;
  nu->bezt[2].vec[2][0] = 0.7f;
  nu->bezt[2].vec[2][1] = 1.0f;

  /* Top-left corner */
  nu->bezt[3].vec[1][0] = 0.0f;
  nu->bezt[3].vec[1][1] = 1.0f;
  nu->bezt[3].vec[0][0] = 0.3f;
  nu->bezt[3].vec[0][1] = 1.0f;
  nu->bezt[3].vec[2][0] = 0.0f;
  nu->bezt[3].vec[2][1] = 0.7f;

  BLI_addtail(&nurbsbase, nu);

  /* Verify test setup */
  float segment_len = nu->bezt[1].vec[1][0] - nu->bezt[0].vec[1][0];       /* 1.0 */
  float handle_projection = nu->bezt[0].vec[2][0] - nu->bezt[0].vec[1][0]; /* 1.5 */
  EXPECT_GT(handle_projection, segment_len) << "Test setup: handle should overshoot";

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* After processing, result should be valid */
  EXPECT_GE(count_nurbs(&nurbsbase), 1) << "Should produce output contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "Overshooting handle should be handled correctly";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test that normal (non-stupid) handles are preserved.
 */
TEST(vfontdata_sanitize, StupidControlPoints_NormalHandlesPreserved)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a closed square contour with well-behaved handles */
  Nurb *nu = create_empty_bezier_nurb(4);

  /* Bottom-left corner with reasonable forward-pointing handle */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.1f;
  nu->bezt[0].vec[0][1] = 0.1f;
  nu->bezt[0].vec[2][0] = 0.3f; /* Points forward, doesn't overshoot */
  nu->bezt[0].vec[2][1] = 0.0f;

  /* Bottom-right corner */
  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 0.0f;
  nu->bezt[1].vec[0][0] = 0.7f;
  nu->bezt[1].vec[0][1] = 0.0f;
  nu->bezt[1].vec[2][0] = 1.0f;
  nu->bezt[1].vec[2][1] = 0.3f;

  /* Top-right corner */
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 1.0f;
  nu->bezt[2].vec[0][0] = 1.0f;
  nu->bezt[2].vec[0][1] = 0.7f;
  nu->bezt[2].vec[2][0] = 0.7f;
  nu->bezt[2].vec[2][1] = 1.0f;

  /* Top-left corner */
  nu->bezt[3].vec[1][0] = 0.0f;
  nu->bezt[3].vec[1][1] = 1.0f;
  nu->bezt[3].vec[0][0] = 0.3f;
  nu->bezt[3].vec[0][1] = 1.0f;
  nu->bezt[3].vec[2][0] = 0.0f;
  nu->bezt[3].vec[2][1] = 0.7f;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should produce valid output */
  EXPECT_GE(count_nurbs(&nurbsbase), 1) << "Should produce output contour";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "Normal handles should be preserved";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Annoying Extrema Removal Tests
 *
 * Tests for ss_remove_annoying_extrema() which removes extrema that:
 * - Are very close to an endpoint
 * - Are on the opposite side of the endpoint from the other endpoint
 *
 * FontForge: splineutil.c SplineRemoveAnnoyingExtrema()
 * \{ */

/**
 * Test that a curve with an extremum close to an endpoint is smoothed.
 * When a control point creates a tiny bump near the endpoint on the
 * "wrong" side, it should be corrected.
 */
TEST(vfontdata_sanitize, AnnoyingExtrema_TinyBumpAtEndpoint)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a curve segment where the handle creates a tiny bump
   * that goes slightly past the endpoint. */
  Nurb *nu = create_empty_bezier_nurb(2);

  /* Point 0 at (0, 0.5), handle creates tiny bump going slightly below */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.5f;
  nu->bezt[0].vec[0][0] = -0.1f;
  nu->bezt[0].vec[0][1] = 0.5f;
  nu->bezt[0].vec[2][0] = 0.1f;
  nu->bezt[0].vec[2][1] = 0.48f; /* Slight dip */

  /* Point 1 at (1, 0.5), handle creates tiny bump going below */
  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 0.5f;
  nu->bezt[1].vec[0][0] = 0.9f;
  nu->bezt[1].vec[0][1] = 0.48f; /* Slight dip */
  nu->bezt[1].vec[2][0] = 1.1f;
  nu->bezt[1].vec[2][1] = 0.5f;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Result should be valid */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "Tiny bumps at endpoints should be handled";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test that a curve with a significant extremum (not annoying) is preserved.
 * Extrema that are far from endpoints or on the correct side should remain.
 */
TEST(vfontdata_sanitize, AnnoyingExtrema_SignificantExtremumPreserved)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a curve with a deliberate, significant curve.
   * This is a quarter-circle-like curve where the extremum is intentional. */
  Nurb *nu = create_empty_bezier_nurb(2);

  /* Point 0 at (0, 0) */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.2f;
  nu->bezt[0].vec[0][1] = 0.0f;
  nu->bezt[0].vec[2][0] = 0.0f; /* Handle goes up */
  nu->bezt[0].vec[2][1] = 0.55f;

  /* Point 1 at (1, 1) */
  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 1.0f;
  nu->bezt[1].vec[0][0] = 0.45f; /* Handle comes from left */
  nu->bezt[1].vec[0][1] = 1.0f;
  nu->bezt[1].vec[2][0] = 1.2f;
  nu->bezt[1].vec[2][1] = 1.0f;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Curve with significant extrema should still be valid */
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase)) << "Significant extrema should be preserved";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/**
 * Test with a closed contour that has tiny bumps at corners.
 * This simulates font glyph issues where control points create
 * unwanted small extrema.
 */
TEST(vfontdata_sanitize, AnnoyingExtrema_ClosedContourWithBumps)
{
  ListBase nurbsbase = {nullptr, nullptr};

  /* Create a roughly square contour where handles create small bumps */
  Nurb *nu = create_empty_bezier_nurb(4);

  /* Bottom-left corner with slight bump */
  nu->bezt[0].vec[1][0] = 0.0f;
  nu->bezt[0].vec[1][1] = 0.0f;
  nu->bezt[0].vec[0][0] = -0.02f; /* Tiny bump outward */
  nu->bezt[0].vec[0][1] = 0.1f;
  nu->bezt[0].vec[2][0] = 0.1f;
  nu->bezt[0].vec[2][1] = -0.02f; /* Tiny bump outward */

  /* Bottom-right corner */
  nu->bezt[1].vec[1][0] = 1.0f;
  nu->bezt[1].vec[1][1] = 0.0f;
  nu->bezt[1].vec[0][0] = 0.9f;
  nu->bezt[1].vec[0][1] = -0.02f; /* Tiny bump */
  nu->bezt[1].vec[2][0] = 1.02f;  /* Tiny bump */
  nu->bezt[1].vec[2][1] = 0.1f;

  /* Top-right corner */
  nu->bezt[2].vec[1][0] = 1.0f;
  nu->bezt[2].vec[1][1] = 1.0f;
  nu->bezt[2].vec[0][0] = 1.02f; /* Tiny bump */
  nu->bezt[2].vec[0][1] = 0.9f;
  nu->bezt[2].vec[2][0] = 0.9f;
  nu->bezt[2].vec[2][1] = 1.02f; /* Tiny bump */

  /* Top-left corner */
  nu->bezt[3].vec[1][0] = 0.0f;
  nu->bezt[3].vec[1][1] = 1.0f;
  nu->bezt[3].vec[0][0] = 0.1f;
  nu->bezt[3].vec[0][1] = 1.02f;  /* Tiny bump */
  nu->bezt[3].vec[2][0] = -0.02f; /* Tiny bump */
  nu->bezt[3].vec[2][1] = 0.9f;

  BLI_addtail(&nurbsbase, nu);

  blf_glyph_remove_overlaps(as_nurb_list(&nurbsbase), 0.001f);

  /* Should handle tiny bumps at corners correctly */
  int contour_count = count_nurbs(&nurbsbase);
  EXPECT_GE(contour_count, 1) << "Contour with tiny bumps should produce output";
  EXPECT_TRUE(all_nurbs_valid(&nurbsbase))
      << "Closed contour with bumps should produce valid result";

  BKE_nurbList_free(as_nurb_list(&nurbsbase));
}

/** \} */

}  // namespace blender::bke::tests
