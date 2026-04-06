/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_vector_compare_lib.glsl"

[[node]]
void axis_angle_to_rotation(float3 axis, float angle, out float4 rotation)
{
  if (is_zero(axis)) {
    rotation = float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  else {
    const float3 naxis = normalize(axis);

    const float half_angle = angle * 0.5f;
    const float hs = sin(half_angle);
    const float hc = cos(half_angle);
    const float3 xyz = naxis * hs;
    rotation = float4(hc, xyz.x, xyz.y, xyz.z);
  }
}
