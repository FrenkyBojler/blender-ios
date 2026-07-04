/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_math_base_c.hh"
#include "BLI_math_geom_c.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector_types.hh"

namespace blender {

TEST(math_geom, DistToLine2DSimple)
{
  float p[2] = {5.0f, 1.0f}, a[2] = {0.0f, 0.0f}, b[2] = {2.0f, 0.0f};
  float distance = dist_to_line_v2(p, a, b);
  EXPECT_NEAR(1.0f, distance, 1e-6);
}

TEST(math_geom, DistToLineSegment2DSimple)
{
  float p[2] = {3.0f, 1.0f}, a[2] = {0.0f, 0.0f}, b[2] = {2.0f, 0.0f};
  float distance = dist_to_line_segment_v2(p, a, b);
  EXPECT_NEAR(sqrtf(2.0f), distance, 1e-6);
}

TEST(math_geom, IsectPointTri2D)
{
  float2 tri_cw[3] = {{-2, 1}, {4, 4}, {2, -3}};
  float2 tri_ccw[3] = {{-2, 1}, {2, -3}, {4, 4}};

  float2 inside1{0, 0};
  float2 inside2{2, 2};
  float2 inside3{2, -1};
  float2 inside4{-1, 1};
  EXPECT_EQ(-1, isect_point_tri_v2(inside1, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(+1, isect_point_tri_v2(inside1, tri_ccw[0], tri_ccw[1], tri_ccw[2]));
  EXPECT_EQ(-1, isect_point_tri_v2(inside2, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(+1, isect_point_tri_v2(inside2, tri_ccw[0], tri_ccw[1], tri_ccw[2]));
  EXPECT_EQ(-1, isect_point_tri_v2(inside3, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(+1, isect_point_tri_v2(inside3, tri_ccw[0], tri_ccw[1], tri_ccw[2]));
  EXPECT_EQ(-1, isect_point_tri_v2(inside4, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(+1, isect_point_tri_v2(inside4, tri_ccw[0], tri_ccw[1], tri_ccw[2]));

  float2 outside1{2, 4};
  float2 outside2{-1, -1};
  float2 outside3{0, 3};
  float2 outside4{-4, 0};
  EXPECT_EQ(0, isect_point_tri_v2(outside1, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(0, isect_point_tri_v2(outside1, tri_ccw[0], tri_ccw[1], tri_ccw[2]));
  EXPECT_EQ(0, isect_point_tri_v2(outside2, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(0, isect_point_tri_v2(outside2, tri_ccw[0], tri_ccw[1], tri_ccw[2]));
  EXPECT_EQ(0, isect_point_tri_v2(outside3, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(0, isect_point_tri_v2(outside3, tri_ccw[0], tri_ccw[1], tri_ccw[2]));
  EXPECT_EQ(0, isect_point_tri_v2(outside4, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(0, isect_point_tri_v2(outside4, tri_ccw[0], tri_ccw[1], tri_ccw[2]));

  float2 edge1{0, 2};
  float2 edge2{1, -2};
  EXPECT_EQ(-1, isect_point_tri_v2(edge1, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(+1, isect_point_tri_v2(edge1, tri_ccw[0], tri_ccw[1], tri_ccw[2]));
  EXPECT_EQ(-1, isect_point_tri_v2(edge2, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(+1, isect_point_tri_v2(edge2, tri_ccw[0], tri_ccw[1], tri_ccw[2]));

  float2 corner1{4, 4};
  float2 corner2{2, -3};
  EXPECT_EQ(-1, isect_point_tri_v2(corner1, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(+1, isect_point_tri_v2(corner1, tri_ccw[0], tri_ccw[1], tri_ccw[2]));
  EXPECT_EQ(-1, isect_point_tri_v2(corner2, tri_cw[0], tri_cw[1], tri_cw[2]));
  EXPECT_EQ(+1, isect_point_tri_v2(corner2, tri_ccw[0], tri_ccw[1], tri_ccw[2]));
}

TEST(math_geom, IsectPointQuad2D)
{
  float2 quad_cw[4] = {{-2, 1}, {4, 4}, {5, 1}, {2, -3}};
  float2 quad_ccw[4] = {{-2, 1}, {2, -3}, {5, 1}, {4, 4}};

  float2 inside1{0, 0};
  float2 inside2{2, 2};
  float2 inside3{3, -1};
  float2 inside4{-1, 1};
  EXPECT_EQ(-1, isect_point_quad_v2(inside1, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(+1, isect_point_quad_v2(inside1, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));
  EXPECT_EQ(-1, isect_point_quad_v2(inside2, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(+1, isect_point_quad_v2(inside2, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));
  EXPECT_EQ(-1, isect_point_quad_v2(inside3, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(+1, isect_point_quad_v2(inside3, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));
  EXPECT_EQ(-1, isect_point_quad_v2(inside4, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(+1, isect_point_quad_v2(inside4, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));

  float2 outside1{2, 4};
  float2 outside2{-1, -1};
  float2 outside3{0, 3};
  float2 outside4{-4, 0};
  EXPECT_EQ(0, isect_point_quad_v2(outside1, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(0, isect_point_quad_v2(outside1, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));
  EXPECT_EQ(0, isect_point_quad_v2(outside2, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(0, isect_point_quad_v2(outside2, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));
  EXPECT_EQ(0, isect_point_quad_v2(outside3, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(0, isect_point_quad_v2(outside3, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));
  EXPECT_EQ(0, isect_point_quad_v2(outside4, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(0, isect_point_quad_v2(outside4, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));

  float2 edge1{0, 2};
  float2 edge2{1, -2};
  EXPECT_EQ(-1, isect_point_quad_v2(edge1, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(+1, isect_point_quad_v2(edge1, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));
  EXPECT_EQ(-1, isect_point_quad_v2(edge2, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(+1, isect_point_quad_v2(edge2, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));

  float2 corner1{4, 4};
  float2 corner2{2, -3};
  EXPECT_EQ(-1, isect_point_quad_v2(corner1, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(+1, isect_point_quad_v2(corner1, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));
  EXPECT_EQ(-1, isect_point_quad_v2(corner2, quad_cw[0], quad_cw[1], quad_cw[2], quad_cw[3]));
  EXPECT_EQ(+1, isect_point_quad_v2(corner2, quad_ccw[0], quad_ccw[1], quad_ccw[2], quad_ccw[3]));
}

TEST(math_geom, CrossPoly)
{
  const float tri_cw_2d[3][2] = {{-1, 0}, {0, 1}, {1, 0}};
  const float tri_cw_3d[3][3] = {{-1, 0}, {0, 1}, {1, 0}};

  const float tri_ccw_2d[3][2] = {{1, 0}, {0, 1}, {-1, 0}};
  const float tri_ccw_3d[3][3] = {{1, 0}, {0, 1}, {-1, 0}};

  auto cross_tri_v3_as_float3 = [](const float (*poly)[3]) -> float3 {
    float n[3];
    cross_tri_v3(n, UNPACK3(poly));
    return float3(n[0], n[1], n[2]);
  };

  auto cross_poly_v3_as_float3 = [](const float (*poly)[3]) -> float3 {
    float n[3];
    cross_poly_v3(n, poly, 3);
    return float3(n[0], n[1], n[2]);
  };

  /* Clockwise. */
  EXPECT_EQ(cross_tri_v3_as_float3(tri_cw_3d)[2], -2);
  EXPECT_EQ(cross_tri_v2(UNPACK3(tri_cw_2d)), -2);

  EXPECT_EQ(cross_poly_v3_as_float3(tri_cw_3d)[2], -2);
  EXPECT_EQ(cross_poly_v2(tri_cw_2d, 3), -2);

  /* Counter clockwise. */
  EXPECT_EQ(cross_tri_v3_as_float3(tri_ccw_3d)[2], 2);
  EXPECT_EQ(cross_tri_v2(UNPACK3(tri_ccw_2d)), 2);

  EXPECT_EQ(cross_poly_v3_as_float3(tri_ccw_3d)[2], 2);
  EXPECT_EQ(cross_poly_v2(tri_ccw_2d, 3), 2);
}

/**
 * Single-plane segment clipping keeps output points valid on success and rejects segments
 * entirely behind the clipping plane.
 */
TEST(math_geom, ClipSegmentV3Plane)
{
  /* Plane x = 0, with positive x in front. */
  const float plane[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  float r_p1[3], r_p2[3];

  {
    /* Crossing the plane: clip the behind endpoint to x = 0. */
    const float p1[3] = {-1.0f, 0.0f, 0.0f};
    const float p2[3] = {1.0f, 0.0f, 0.0f};
    const float expect_p1[3] = {0.0f, 0.0f, 0.0f};
    EXPECT_TRUE(clip_segment_v3_plane(p1, p2, plane, r_p1, r_p2));
    EXPECT_V3_NEAR(expect_p1, r_p1, 1e-6f);
    EXPECT_V3_NEAR(p2, r_p2, 1e-6f);
  }

  {
    /* Parallel and in front of the plane: keep the segment unchanged. */
    const float p1[3] = {1.0f, -1.0f, 0.0f};
    const float p2[3] = {1.0f, 1.0f, 0.0f};
    EXPECT_TRUE(clip_segment_v3_plane(p1, p2, plane, r_p1, r_p2));
    EXPECT_V3_NEAR(p1, r_p1, 1e-6f);
    EXPECT_V3_NEAR(p2, r_p2, 1e-6f);
  }

  {
    /* Parallel and behind the plane: reject the segment. */
    const float p1[3] = {-1.0f, -1.0f, 0.0f};
    const float p2[3] = {-1.0f, 1.0f, 0.0f};
    EXPECT_FALSE(clip_segment_v3_plane(p1, p2, plane, r_p1, r_p2));
  }
}

/**
 * Regression for #160753: perspective snap on long loose edges.
 *
 * When part of a thin world-space AABB is behind the camera, projected AABB
 * culling must not return a bogus distance in the hundreds of pixels.
 */
TEST(math_geom, DistSquaredToProjectedAabb_LooseEdgeBehindCamera)
{
  const float winsize[2] = {1920.0f, 1080.0f};
  const float mval[2] = {964.0f, 491.0f};

  /* Long thin bound-box of a subdivided loose edge chain (meters). */
  const float bbmin[3] = {0.0f, -0.01f, -0.01f};
  const float bbmax[3] = {25.0f, 0.01f, 0.01f};

  /* View close to the edge start, rotated in perspective (matches reporter setup). */
  float viewmat[4][4];
  unit_m4(viewmat);
  translate_m4(viewmat, -2.0f, -6.0f, 2.5f);
  rotate_m4(viewmat, 'Z', 0.35f);
  rotate_m4(viewmat, 'X', -0.65f);

  /* Perspective projection. */
  float winmat[4][4];
  perspective_m4(winmat, -0.8f, 0.8f, -0.45f, 0.45f, 0.1f, 1000.0f);

  /* Full world-to-screen matrix passed to snap boundbox culling. */
  float persmat[4][4];
  mul_m4_m4m4(persmat, winmat, viewmat);

  const float dist_sq = dist_squared_to_projected_aabb_simple(
      persmat, winsize, mval, bbmin, bbmax);

  /* The selected AABB edge may not be safe to project while other corners remain visible;
   * returning a conservative distance avoids a false snap rejection. */
  const float snap_threshold_sq = square_f(30.0f);
  EXPECT_LE(dist_sq, snap_threshold_sq);
}

/**
 * Near frustum data is computed once in #dist_squared_to_projected_aabb_precalc.
 */
TEST(math_geom, DistSquaredToProjectedAabb_PrecalcNearPlane)
{
  const float winsize[2] = {1920.0f, 1080.0f};
  const float mval[2] = {960.0f, 540.0f};

  /* Keep the view matrix simple so known points can be tested against the near plane. */
  float viewmat[4][4];
  unit_m4(viewmat);

  /* Build a perspective frustum with near plane at 0.1 and far plane at 1000. */
  const float near_clip = 0.1f;
  float winmat[4][4];
  perspective_m4(winmat, -0.8f, 0.8f, -0.45f, 0.45f, near_clip, 1000.0f);

  float persmat[4][4];
  mul_m4_m4m4(persmat, winmat, viewmat);

  DistProjectedAABBPrecalc precalc;
  dist_squared_to_projected_aabb_precalc(&precalc, persmat, winsize, mval);

  /* A matrix built from a perspective frustum should be detected as perspective. */
  EXPECT_TRUE(precalc.is_persp);

  /* The stored near plane should separate clipped points from visible points. */
  const float behind_near[3] = {0.0f, 0.0f, 0.0f};
  const float on_near[3] = {0.0f, 0.0f, -near_clip};
  const float in_front_of_near[3] = {0.0f, 0.0f, -1.0f};
  EXPECT_LT(plane_point_side_v3(precalc.near_plane, behind_near), 0.0f);
  EXPECT_NEAR(plane_point_side_v3(precalc.near_plane, on_near), 0.0f, 1e-5f);
  EXPECT_GT(plane_point_side_v3(precalc.near_plane, in_front_of_near), 0.0f);

  /* Orthographic-like matrix: projected `w` is constant, so no perspective near clip is needed. */
  float ortho_winmat[4][4];
  unit_m4(ortho_winmat);
  ortho_winmat[0][0] = 1.0f / 960.0f;
  ortho_winmat[1][1] = 1.0f / 540.0f;
  mul_m4_m4m4(persmat, ortho_winmat, viewmat);
  dist_squared_to_projected_aabb_precalc(&precalc, persmat, winsize, mval);
  EXPECT_FALSE(precalc.is_persp);
}
}  // namespace blender
