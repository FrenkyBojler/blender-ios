/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void rotation_to_axis_angle(float4 rotation, out float3 axis, out float angle)
{
  axis = float3(rotation.y, rotation.z, rotation.w);
  const float cos_half_angle = rotation.x;
  const float sin_half_angle = length(axis);

  if (sin_half_angle < 0.0005f) {
    const float3 identity_axis = float3(0.0f, 1.0f, 0.0f);
    axis = identity_axis * sign(cos_half_angle);
    angle = 0.0f;
    return;
  }

  axis /= sin_half_angle;
  angle = 2.0f * atan(sin_half_angle, cos_half_angle);
}
