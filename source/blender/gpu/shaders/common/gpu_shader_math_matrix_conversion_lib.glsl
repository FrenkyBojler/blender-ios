/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_euler_lib.glsl"
#include "gpu_shader_math_matrix_compare_lib.glsl"
#include "gpu_shader_math_quaternion_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

/* -------------------------------------------------------------------- */
/** \name Conversion function.
 * \{ */

namespace detail {

Quaternion normalized_to_quat_fast(float3x3 mat)
{
  /* Caller must ensure matrices aren't negative for valid results, see: #24291, #94231. */
  Quaternion q;

  /* Method outlined by Mike Day, ref: https://math.stackexchange.com/a/3183435/220949
   * with an additional `sqrtf(..)` for higher precision result.
   * Removing the `sqrt` causes tests to fail unless the precision is set to 1e-6f or larger. */

  if (mat[2][2] < 0.0f) {
    if (mat[0][0] > mat[1][1]) {
      float trace = 1.0f + mat[0][0] - mat[1][1] - mat[2][2];
      float s = 2.0f * sqrt(trace);
      if (mat[1][2] < mat[2][1]) {
        /* Ensure W is non-negative for a canonical result. */
        s = -s;
      }
      q.y = 0.25f * s;
      s = 1.0f / s;
      q.x = (mat[1][2] - mat[2][1]) * s;
      q.z = (mat[0][1] + mat[1][0]) * s;
      q.w = (mat[2][0] + mat[0][2]) * s;
      if ((trace == 1.0f) && (q.x == 0.0f && q.z == 0.0f && q.w == 0.0f)) {
        /* Avoids the need to normalize the degenerate case. */
        q.y = 1.0f;
      }
    }
    else {
      float trace = 1.0f - mat[0][0] + mat[1][1] - mat[2][2];
      float s = 2.0f * sqrt(trace);
      if (mat[2][0] < mat[0][2]) {
        /* Ensure W is non-negative for a canonical result. */
        s = -s;
      }
      q.z = 0.25f * s;
      s = 1.0f / s;
      q.x = (mat[2][0] - mat[0][2]) * s;
      q.y = (mat[0][1] + mat[1][0]) * s;
      q.w = (mat[1][2] + mat[2][1]) * s;
      if ((trace == 1.0f) && (q.x == 0.0f && q.y == 0.0f && q.w == 0.0f)) {
        /* Avoids the need to normalize the degenerate case. */
        q.z = 1.0f;
      }
    }
  }
  else {
    if (mat[0][0] < -mat[1][1]) {
      float trace = 1.0f - mat[0][0] - mat[1][1] + mat[2][2];
      float s = 2.0f * sqrt(trace);
      if (mat[0][1] < mat[1][0]) {
        /* Ensure W is non-negative for a canonical result. */
        s = -s;
      }
      q.w = 0.25f * s;
      s = 1.0f / s;
      q.x = (mat[0][1] - mat[1][0]) * s;
      q.y = (mat[2][0] + mat[0][2]) * s;
      q.z = (mat[1][2] + mat[2][1]) * s;
      if ((trace == 1.0f) && (q.x == 0.0f && q.y == 0.0f && q.z == 0.0f)) {
        /* Avoids the need to normalize the degenerate case. */
        q.w = 1.0f;
      }
    }
    else {
      /* NOTE(@ideasman42): A zero matrix will fall through to this block,
       * needed so a zero scaled matrices to return a quaternion without rotation, see: #101848. */
      float trace = 1.0f + mat[0][0] + mat[1][1] + mat[2][2];
      float s = 2.0f * sqrt(trace);
      q.x = 0.25f * s;
      s = 1.0f / s;
      q.y = (mat[1][2] - mat[2][1]) * s;
      q.z = (mat[2][0] - mat[0][2]) * s;
      q.w = (mat[0][1] - mat[1][0]) * s;
      if ((trace == 1.0f) && (q.y == 0.0f && q.z == 0.0f && q.w == 0.0f)) {
        /* Avoids the need to normalize the degenerate case. */
        q.x = 1.0f;
      }
    }
  }
  return q;
}

Quaternion normalized_to_quat_with_checks(float3x3 mat)
{
  float det = determinant(mat);
  if (!isfinite(det)) {
    return Quaternion::identity();
  }
  if (det < 0.0f) {
    return normalized_to_quat_fast(-mat);
  }
  return normalized_to_quat_fast(mat);
}

void normalized_to_eul2(float3x3 mat, out EulerXYZ eul1, out EulerXYZ eul2)
{
  float cy = hypot(mat[0][0], mat[0][1]);
  if (cy > 16.0f * FLT_EPSILON) {
    eul1.x = atan2(mat[1][2], mat[2][2]);
    eul1.y = atan2(-mat[0][2], cy);
    eul1.z = atan2(mat[0][1], mat[0][0]);

    eul2.x = atan2(-mat[1][2], -mat[2][2]);
    eul2.y = atan2(-mat[0][2], -cy);
    eul2.z = atan2(-mat[0][1], -mat[0][0]);
  }
  else {
    eul1.x = atan2(-mat[2][1], mat[1][1]);
    eul1.y = atan2(-mat[0][2], cy);
    eul1.z = 0.0f;

    eul2 = eul1;
  }
}
};  // namespace detail

/**
 * Extract the absolute 3d scale from a transform matrix.
 */
float3 to_scale(float3x3 mat)
{
  return float3(length(mat[0]), length(mat[1]), length(mat[2]));
}
/**
 * Extract the absolute 3d scale from a transform matrix.
 */
float3 to_scale(float4x4 mat)
{
  return to_scale(to_float3x3(mat));
}
/**
 * Extract the absolute 3d scale from a transform matrix.
 */
template<typename MatT, bool allow_negative_scale> float3 to_scale(MatT mat)
{
  float3 result = to_scale(mat);
  if (allow_negative_scale) {
    if (is_negative(mat)) {
      result = -result;
    }
  }
  return result;
}
template float3 to_scale<float3x3, true>(float3x3 mat);
template float3 to_scale<float3x3, false>(float3x3 mat);
template float3 to_scale<float4x4, true>(float4x4 mat);
template float3 to_scale<float4x4, false>(float4x4 mat);

#if 0 /* Remove unused variants as they are slow down compilation. */
/**
 * Decompose a matrix into location, rotation, and scale components.
 * \tparam allow_negative_scale: if true, will compute determinant to know if matrix is negative.
 * Rotation and scale values will be flipped if it is negative.
 * This is a costly operation so it is disabled by default.
 */
void to_rot_scale(float3x3 mat, out EulerXYZ r_rotation, out float3 r_scale)
{
  r_scale = to_scale(mat);
  r_rotation = to_euler(mat, true);
}

/**
 * Decompose a matrix into location, rotation, and scale components.
 * \tparam allow_negative_scale: if true, will compute determinant to know if matrix is negative.
 * Rotation and scale values will be flipped if it is negative.
 * This is a costly operation so it is disabled by default.
 */
void to_rot_scale(float3x3 mat,
                  out EulerXYZ r_rotation,
                  out float3 r_scale,
                  const bool allow_negative_scale)
{
  float3x3 normalized_mat = normalize_and_get_size(mat, r_scale);
  if (allow_negative_scale) {
    if (is_negative(normalized_mat)) {
      normalized_mat = -normalized_mat;
      r_scale = -r_scale;
    }
  }
  r_rotation = to_euler(mat, true);
}
void to_rot_scale(float3x3 mat, out Quaternion r_rotation, out float3 r_scale)
{
  r_scale = to_scale(mat);
  r_rotation = to_quaternion(mat, true);
}
void to_rot_scale(float3x3 mat,
                  out Quaternion r_rotation,
                  out float3 r_scale,
                  const bool allow_negative_scale)
{
  float3x3 normalized_mat = normalize_and_get_size(mat, r_scale);
  if (allow_negative_scale) {
    if (is_negative(normalized_mat)) {
      normalized_mat = -normalized_mat;
      r_scale = -r_scale;
    }
  }
  r_rotation = to_quaternion(mat, true);
}

void to_loc_rot_scale(float4x4 mat,
                      out float3 r_location,
                      out EulerXYZ r_rotation,
                      out float3 r_scale)
{
  r_location = mat[3].xyz;
  to_rot_scale(to_float3x3(mat), r_rotation, r_scale);
}
void to_loc_rot_scale(float4x4 mat,
                      out float3 r_location,
                      out EulerXYZ r_rotation,
                      out float3 r_scale,
                      const bool allow_negative_scale)
{
  r_location = mat[3].xyz;
  to_rot_scale(to_float3x3(mat), r_rotation, r_scale, allow_negative_scale);
}
void to_loc_rot_scale(float4x4 mat,
                      out float3 r_location,
                      out Quaternion r_rotation,
                      out float3 r_scale)
{
  r_location = mat[3].xyz;
  to_rot_scale(to_float3x3(mat), r_rotation, r_scale);
}
void to_loc_rot_scale(float4x4 mat,
                      out float3 r_location,
                      out Quaternion r_rotation,
                      out float3 r_scale,
                      const bool allow_negative_scale)
{
  r_location = mat[3].xyz;
  to_rot_scale(to_float3x3(mat), r_rotation, r_scale, allow_negative_scale);
}
#endif

/** \} */
