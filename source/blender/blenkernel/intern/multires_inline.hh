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

namespace blender {

static float euclidean_norm(const blender::float3x3 mat)
{
  blender::Span<float> values(mat.base_ptr(), 9);
  float sum = 0.0f;
  for (int i = 0; i < 9; i++) {
    sum += values[i] * values[i];
  }
  return sqrt(sum);
}

BLI_INLINE void BKE_multires_construct_tangent_matrix(blender::float3x3 &tangent_matrix,
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

  /* Do cross product in double precision due to possibility of nearly parallel partial derivative
   * tangent vectors */
  blender::float3 N = blender::float3(blender::math::normalize(blender::math::cross(
      blender::double3(tangent_matrix.x_axis()), blender::double3(tangent_matrix.y_axis()))));

  constexpr float eps = 0.000001f;
  /* Check for a bad cross product by inspecting the length, if within this arbitrary epsilon,
   * return the null matrix. */
  if (blender::math::length_squared(N) < eps) {
    tangent_matrix = blender::float3x3::zero();
    return;
  }

  tangent_matrix.z_axis() = N;

  const float angle_between = RAD2DEGF(blender::math::acos(
    blender::math::dot(tangent_matrix.x_axis(), tangent_matrix.y_axis()) /
    (blender::math::length(tangent_matrix.x_axis() *
                           blender::math::length(tangent_matrix.y_axis())))));

  /* Adjust the angle by a small amount to not have nearly parallel lines */
  constexpr float threshold = 85.0f;
  constexpr float low_threshold = 90.0f - threshold;
  constexpr float high_threshold = 90.0f + threshold;

  if (angle_between < low_threshold) {
    const float deg_to_rotate = low_threshold - angle_between / 2.0f;
    const float rad_to_rotate = DEG2RADF(deg_to_rotate);
    tangent_matrix.x_axis() = blender::math::rotate_around_axis(
        tangent_matrix.x_axis(), blender::float3(0.0f), tangent_matrix.z_axis(), -rad_to_rotate);
    tangent_matrix.y_axis() = blender::math::rotate_around_axis(
        tangent_matrix.y_axis(), blender::float3(0.0f), tangent_matrix.z_axis(), rad_to_rotate);
  }
  else if (angle_between > high_threshold) {
    const float deg_to_rotate = angle_between - high_threshold / 2.0f;
    const float rad_to_rotate = DEG2RADF(deg_to_rotate);
    tangent_matrix.x_axis() = blender::math::rotate_around_axis(
        tangent_matrix.x_axis(), blender::float3(0.0f), tangent_matrix.z_axis(), rad_to_rotate);
    tangent_matrix.y_axis() = blender::math::rotate_around_axis(
        tangent_matrix.y_axis(), blender::float3(0.0f), tangent_matrix.z_axis(), -rad_to_rotate);
  }

  const float geometric_mean = blender::math::sqrt(blender::math::length(tangent_matrix.x_axis()) *
                                                   blender::math::length(tangent_matrix.y_axis()));

  tangent_matrix.x_axis() = tangent_matrix.x_axis();
  tangent_matrix.y_axis() = tangent_matrix.y_axis();
  tangent_matrix.z_axis() = blender::math::normalize(N) * geometric_mean;

  const blender::float3x3 inv_mat = blender::math::invert(tangent_matrix);
  const float condition_number = euclidean_norm(tangent_matrix) * euclidean_norm(inv_mat);
  /* This is a pretty aggressive number, but the vast majority (99%) of the vertices on a human
   * mesh have a value close to 3.0 */
  if (condition_number > 10.0f) {
    tangent_matrix = blender::float3x3::zero();
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

  tangent_matrix.x_axis() = math::normalize(tangent_matrix.x_axis());
  tangent_matrix.y_axis() = math::normalize(tangent_matrix.y_axis());
  tangent_matrix.z_axis() = math::normalize(tangent_matrix.z_axis());
}

}  // namespace blender
