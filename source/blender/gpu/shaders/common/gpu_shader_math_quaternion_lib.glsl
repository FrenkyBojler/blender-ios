/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_shader_compat.hh"

struct Quaternion {
  float x, y, z, w;

  static Quaternion identity()
  {
    return {1, 0, 0, 0};
  }

  float4 as_float4() const
  {
    return float4(this->x, this->y, this->z, this->w);
  }
};

/* -------------------------------------------------------------------- */
/** \name Quaternion Math
 * \{ */

float4 math_quaternion_multiply(float4 a, float4 b)
{
  return float4(a.x * b.x - a.y * b.y - a.z * b.z - a.w * b.w,
                a.x * b.y + a.y * b.x + a.z * b.w - a.w * b.z,
                a.x * b.z - a.y * b.w + a.z * b.x + a.w * b.y,
                a.x * b.w + a.y * b.z - a.z * b.y + a.w * b.x);
}

float3 transform_point_by_quaternion(float4 q, float3 v)
{
  const float S_w = -q.y * v.x - q.z * v.y - q.w * v.z;
  const float S_x = q.x * v.x + q.z * v.z - q.w * v.y;
  const float S_y = q.x * v.y + q.w * v.x - q.y * v.z;
  const float S_z = q.x * v.z + q.y * v.y - q.z * v.x;
  float3 R;
  R.x = S_w * (-q.y) + S_x * q.x - S_y * q.w + S_z * q.z;
  R.y = S_w * (-q.z) + S_y * q.x - S_z * q.y + S_x * q.w;
  R.z = S_w * (-q.w) + S_z * q.x - S_x * q.z + S_y * q.y;
  return R;
}

/** \} */
