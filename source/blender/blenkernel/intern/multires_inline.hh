/* SPDX-FileCopyrightText: 2018 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "BKE_multires.hh"
#include "BLI_math_constants.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_rotation_legacy.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#define ADJUST_ANGLE 1
#define ORTHOGONALIZE 0
#define NORMALIZE 0
#define CHECK_COND_VALUE 1

static float euclidean_norm_internal(const blender::float3x3 mat)
{
  blender::Span<float> values(mat.base_ptr(), 9);
  float sum = 0.0f;
  for (int i = 0; i < 9; i++) {
    sum += values[i] * values[i];
  }
  return sqrt(sum);
}

BLI_INLINE blender::float3x3 BKE_multires_construct_tangent_matrix(const blender::float3 &dPdu,
                                                                   const blender::float3 &dPdv,
                                                                   const int corner)
{
  blender::float3x3 tangent_matrix;
  if (corner == 0) {
    tangent_matrix.x_axis() = dPdv * -1.0f;
    tangent_matrix.y_axis() = dPdu * -1.0f;
  }
  else if (corner == 1) {
    tangent_matrix.x_axis() = dPdu;
    tangent_matrix.y_axis() = dPdv * -1.0f;
  }
  else if (corner == 2) {
    tangent_matrix.x_axis() = dPdv;
    tangent_matrix.y_axis() = dPdu;
  }
  else if (corner == 3) {
    tangent_matrix.x_axis() = dPdu * -1.0f;
    tangent_matrix.y_axis() = dPdv;
  }
  else {
    BLI_assert_msg(0, "Unhandled corner index");
  }
  /* Do cross product in double precision due to possibility of nearly parallel partial derivative
   * tangent vectors */
  double length;
  blender::float3 N = blender::float3(blender::math::normalize_and_get_length(blender::math::cross(
      blender::double3(tangent_matrix.x_axis()), blender::double3(tangent_matrix.y_axis())), length));

  /* Chosen arbitrarily */
  constexpr float eps = 0.000001f;
  if (blender::math::length_squared(N) < eps) {
    tangent_matrix = blender::float3x3::zero();
    return tangent_matrix;
  }
  else {

    BLI_assert(blender::math::length_squared(N) >= eps);

    tangent_matrix.z_axis() = N;
#if ADJUST_ANGLE
    const float denominator = blender::math::length(tangent_matrix.x_axis()) *
                              blender::math::length(tangent_matrix.y_axis());
    const float angle_between = RAD2DEGF(blender::math::asin(float(length) / denominator));

    constexpr float threshold_angle = 60.0f;
    constexpr float low_threshold = 90.0f - threshold_angle;
    constexpr float high_threshold = 90.0f + threshold_angle;

    if (angle_between < low_threshold) {
      const float deg_to_rotate = low_threshold - angle_between / 2.0f;
      const float rad_to_rotate = DEG2RADF(deg_to_rotate);
      tangent_matrix.x_axis() = blender::math::rotate_around_axis(
          tangent_matrix.x_axis(), blender::float3(0.0f), tangent_matrix.z_axis(), -rad_to_rotate);
      tangent_matrix.y_axis() = blender::math::rotate_around_axis(
          tangent_matrix.y_axis(), blender::float3(0.0f), tangent_matrix.z_axis(), rad_to_rotate);
      if (blender::math::is_zero(tangent_matrix.x_axis()) ||
          blender::math::is_zero(tangent_matrix.y_axis()))
      {
        printf("%f, (%f %f %f), (%f, %f, %f)\n",
               deg_to_rotate,
               tangent_matrix.x_axis().x,
               tangent_matrix.x_axis().y,
               tangent_matrix.x_axis().z,
               tangent_matrix.y_axis().x,
               tangent_matrix.y_axis().y,
               tangent_matrix.y_axis().z);
        BLI_assert_unreachable();
      }
    }
    else if (angle_between > high_threshold) {
      const float deg_to_rotate = angle_between - high_threshold / 2.0f;
      const float rad_to_rotate = DEG2RADF(deg_to_rotate);
      tangent_matrix.x_axis() = blender::math::rotate_around_axis(
          tangent_matrix.x_axis(), blender::float3(0.0f), tangent_matrix.z_axis(), rad_to_rotate);
      tangent_matrix.y_axis() = blender::math::rotate_around_axis(
          tangent_matrix.y_axis(), blender::float3(0.0f), tangent_matrix.z_axis(), -rad_to_rotate);
      if (blender::math::is_zero(tangent_matrix.x_axis()) ||
          blender::math::is_zero(tangent_matrix.y_axis()))
      {
        printf("%f, (%f %f %f), (%f, %f, %f)\n",
               deg_to_rotate,
               tangent_matrix.x_axis().x,
               tangent_matrix.x_axis().y,
               tangent_matrix.x_axis().z,
               tangent_matrix.y_axis().x,
               tangent_matrix.y_axis().y,
               tangent_matrix.y_axis().z);
        BLI_assert_unreachable();
      }
    }
#endif

    float geometric_mean = blender::math::sqrt(blender::math::length(dPdu) *
                                               blender::math::length(dPdv));
    tangent_matrix.z_axis() = blender::math::normalize(tangent_matrix.z_axis()) * geometric_mean;
    const blender::float3x3 inv = blender::math::invert(tangent_matrix);
    if (blender::math::is_zero(tangent_matrix.x_axis()) ||
        blender::math::is_zero(tangent_matrix.y_axis()) ||
        blender::math::is_zero(tangent_matrix.z_axis()))
    {
      printf("Angle Between: %f, N: (%f %f %f) N length sq: %.15f\n", angle_between, N.x, N.y, N.z, blender::math::length_squared(N));
      printf("(%f, %f, %f) x (%f, %f, %f)\n", dPdu.x, dPdu.y, dPdu.z, dPdv.x, dPdv.y, dPdv.z);
      printf("%.15f %.15f %.15f\n",
             tangent_matrix.x_axis().x,
             tangent_matrix.x_axis().y,
             tangent_matrix.x_axis().z);
      printf("%.15f %.15f %.15f\n",
             tangent_matrix.y_axis().x,
             tangent_matrix.y_axis().y,
             tangent_matrix.y_axis().z);
      printf("%.15f %.15f %.15f\n",
             tangent_matrix.z_axis().x,
             tangent_matrix.z_axis().y,
             tangent_matrix.z_axis().z);
      BLI_assert(false);
    }
    BLI_assert(!blender::math::is_zero(tangent_matrix.x_axis()));
    BLI_assert(!blender::math::is_zero(tangent_matrix.y_axis()));
    BLI_assert(!blender::math::is_zero(tangent_matrix.z_axis()));
    if (euclidean_norm_internal(tangent_matrix) * euclidean_norm_internal(inv) > 10) {
      tangent_matrix = blender::float3x3::zero();
    }
    return tangent_matrix;
  }
}

BLI_INLINE void BKE_multires_construct_tangent_matrix_for_versioning(
    blender::float3x3 &tangent_matrix,
    const blender::float3 &dPdu,
    const blender::float3 &dPdv,
    const int corner)
{
  if (corner == 0) {
    tangent_matrix.x_axis() = dPdv * -1.0f;
    tangent_matrix.y_axis() = dPdu * -1.0f;
  }
  else if (corner == 1) {
    tangent_matrix.x_axis() = dPdu;
    tangent_matrix.y_axis() = dPdv * -1.0f;
  }
  else if (corner == 2) {
    tangent_matrix.x_axis() = dPdv;
    tangent_matrix.y_axis() = dPdu;
  }
  else if (corner == 3) {
    tangent_matrix.x_axis() = dPdu * -1.0f;
    tangent_matrix.y_axis() = dPdv;
  }
  else {
    BLI_assert_msg(0, "Unhandled corner index");
  }
  tangent_matrix.z_axis() = blender::math::cross(dPdu, dPdv);

  tangent_matrix.x_axis() = blender::math::normalize(tangent_matrix.x_axis());
  tangent_matrix.y_axis() = blender::math::normalize(tangent_matrix.y_axis());
  tangent_matrix.z_axis() = blender::math::normalize(tangent_matrix.z_axis());
}
