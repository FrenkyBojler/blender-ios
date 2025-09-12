/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_math_axis_angle_lib.glsl"
#include "gpu_shader_math_euler_lib.glsl"
#include "gpu_shader_math_matrix_conversion_lib.glsl"
#include "gpu_shader_math_quaternion_lib.glsl"

/* -------------------------------------------------------------------- */
/** \name Quaternion Functions
 * \{ */

Quaternion to_quaternion(EulerXYZ eul)
{
  float ti = eul.x * 0.5f;
  float tj = eul.y * 0.5f;
  float th = eul.z * 0.5f;
  float ci = cos(ti);
  float cj = cos(tj);
  float ch = cos(th);
  float si = sin(ti);
  float sj = sin(tj);
  float sh = sin(th);
  float cc = ci * ch;
  float cs = ci * sh;
  float sc = si * ch;
  float ss = si * sh;

  Quaternion quat;
  quat.x = cj * cc + sj * ss;
  quat.y = cj * sc - sj * cs;
  quat.z = cj * ss + sj * cc;
  quat.w = cj * cs - sj * sc;
  return quat;
}

/**
 * Extract quaternion rotation from transform matrix.
 * \note normalized is set to false by default.
 */
Quaternion to_quaternion(float3x3 mat)
{
  return detail::normalized_to_quat_with_checks(normalize(mat));
}
/**
 * Extract quaternion rotation from transform matrix.
 * \note normalized is set to false by default.
 */
Quaternion to_quaternion(float3x3 mat, const bool normalized)
{
  if (!normalized) {
    mat = normalize(mat);
  }
  return to_quaternion(mat);
}
/**
 * Extract quaternion rotation from transform matrix.
 * \note normalized is set to false by default.
 */
Quaternion to_quaternion(float4x4 mat)
{
  return to_quaternion(to_float3x3(mat));
}
/**
 * Extract quaternion rotation from transform matrix.
 * \note normalized is set to false by default.
 */
Quaternion to_quaternion(float4x4 mat, const bool normalized)
{
  return to_quaternion(to_float3x3(mat), normalized);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Euler Functions
 * \{ */

/**
 * Extract euler rotation from transform matrix.
 * \return the rotation with the smallest values from the potential candidates.
 */
EulerXYZ to_euler(float3x3 mat, const bool normalized)
{
  if (!normalized) {
    mat = normalize(mat);
  }
  EulerXYZ eul1, eul2;
  detail::normalized_to_eul2(mat, eul1, eul2);
  /* Return best, which is just the one with lowest values it in. */
  return (length_manhattan(eul1.as_float3()) > length_manhattan(eul2.as_float3())) ? eul2 : eul1;
}
/**
 * Extract euler rotation from transform matrix.
 * \return the rotation with the smallest values from the potential candidates.
 */
EulerXYZ to_euler(float3x3 mat)
{
  return to_euler(mat, true);
}
/**
 * Extract euler rotation from transform matrix.
 * \return the rotation with the smallest values from the potential candidates.
 */
EulerXYZ to_euler(float4x4 mat, const bool normalized)
{
  return to_euler(to_float3x3(mat), normalized);
}
/**
 * Extract euler rotation from transform matrix.
 * \return the rotation with the smallest values from the potential candidates.
 */
EulerXYZ to_euler(float4x4 mat)
{
  return to_euler(to_float3x3(mat));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Axis Angle Functions
 * \{ */

Quaternion to_axis_angle(AxisAngle axis_angle)
{
  float angle_cos = cos(axis_angle.angle);
  /** Using half angle identities: sin(angle / 2) = sqrt((1 - angle_cos) / 2) */
  float sine = sqrt(0.5f - angle_cos * 0.5f);
  float cosine = sqrt(0.5f + angle_cos * 0.5f);

  /* TODO(fclem): Optimize. */
  float angle_sin = sin(axis_angle.angle);
  if (angle_sin < 0.0f) {
    sine = -sine;
  }

  Quaternion quat;
  quat.x = cosine;
  quat.y = axis_angle.axis.x * sine;
  quat.z = axis_angle.axis.y * sine;
  quat.w = axis_angle.axis.z * sine;
  return quat;
}

AxisAngle to_axis_angle(Quaternion quat)
{
  /* Calculate angle/2, and sin(angle/2). */
  float ha = acos(quat.x);
  float si = sin(ha);

  /* From half-angle to angle. */
  float angle = ha * 2;
  /* Prevent division by zero for axis conversion. */
  if (abs(si) < 0.0005f) {
    si = 1.0f;
  }

  float3 axis = float3(quat.y, quat.z, quat.w) / si;
  if (is_zero(axis)) {
    axis[1] = 1.0f;
  }
  return AxisAngle(axis, angle);
}

AxisAngle to_axis_angle(EulerXYZ eul)
{
  /* Use quaternions as intermediate representation for now... */
  return to_axis_angle(to_quaternion(eul));
}

/** \} */
