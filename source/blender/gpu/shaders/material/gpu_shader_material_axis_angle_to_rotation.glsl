/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void axis_angle_to_rotation(float3 axis, float angle, out float4 rotation)
{
  const float len = length(axis);
  if (len == 0.0f) {
    rotation = float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  else {
    const float3 naxis = axis / len;

    const float angle_cos = cos(angle);
    /** Using half angle identities: sin(angle / 2) = sqrt((1 - angle_cos) / 2) */
    float sine = sqrt(0.5f - angle_cos * 0.5f);
    const float cosine = sqrt(0.5f + angle_cos * 0.5f);

    const float angle_sin = sin(angle);
    if (angle_sin < 0.0f) {
      sine = -sine;
    }

    rotation = float4(cosine, naxis.x * sine, naxis.y * sine, naxis.z * sine);
  }
}
