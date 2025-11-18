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
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#define ORTHOGONALIZE 0
#define NORMALIZE 0
#define CHECK_COND_VALUE 0

static float euclidean_norm_internal(const blender::float3x3 mat)
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

  /* Chosen arbitrarily */
  constexpr float eps = 0.000001f;
  if (blender::math::length_squared(N) < eps) {
    tangent_matrix = blender::float3x3::zero();
    return;
  }

  tangent_matrix.z_axis() = N;

#  if NORMALIZE
  tangent_matrix.x_axis() = blender::math::normalize(tangent_matrix.x_axis());
  tangent_matrix.y_axis() = blender::math::normalize(tangent_matrix.y_axis());
  tangent_matrix.z_axis() = blender::math::normalize(tangent_matrix.z_axis());
#  else
  float geometric_mean = blender::math::sqrt(blender::math::length(dPdu) *
                                             blender::math::length(dPdv));
  tangent_matrix.x_axis() = tangent_matrix.x_axis();
  tangent_matrix.y_axis() = tangent_matrix.y_axis();
  tangent_matrix.z_axis() = blender::math::normalize(tangent_matrix.z_axis()) * geometric_mean;
#  endif
#  if ORTHOGONALIZE
  tangent_matrix = blender::math::orthogonalize(tangent_matrix, blender::math::Axis::Z);
#  endif
#if CHECK_COND_VALUE
  const blender::float3x3 inv = blender::math::invert(tangent_matrix);
  if (euclidean_norm_internal(tangent_matrix) * euclidean_norm_internal(inv) > 10) {
    tangent_matrix = blender::float3x3::zero();
  }
#endif
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
