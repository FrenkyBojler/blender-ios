/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void rotation_to_axis_angle(float4 rotation, out float3 axis, out float angle)
{
  /* Calculate angle/2, and sin(angle/2). */
  float ha = acos(rotation.x);
  float si = sin(ha);

  /* From half-angle to angle. */
  angle = ha * 2.0;

  /* Prevent division by zero for axis conversion. */
  if (abs(si) < 0.0005f) {
    axis = float3(0.0, 0.0, 1.0);
  }
  else {
    axis = float3(rotation.y, rotation.z, rotation.w) / si;
  }
}
