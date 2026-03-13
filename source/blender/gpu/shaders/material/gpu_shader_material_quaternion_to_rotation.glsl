/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void quaternion_to_rotation(float w, float x, float y, float z, out float4 rotation)
{
  float4 q = float4(w, x, y, z);
  float len = length(q);
  if (len != 0.0) {
    rotation = q / len;
  }
  else {
    rotation = float4(1.0, 0.0, 0.0, 0.0);
  }
}
