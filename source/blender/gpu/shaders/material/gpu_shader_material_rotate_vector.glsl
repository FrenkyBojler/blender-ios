/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void rotate_vector(float3 vector, float4 rotation, float3 &result)
{
  /*
   * Optimized formula used:
   *   v' = v + w * t + cross(qv, t)
   *   where t = 2 * cross(qv, v)
   */
  float3 qv = rotation.yzw;
  float3 t = 2.0 * cross(qv, vector);
  result = vector + rotation.x * t + cross(qv, t);
}
