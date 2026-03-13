/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

float4 quaternion_multiply_float4(float4 a, float4 b)
{
  return float4(a.x * b.x - a.y * b.y - a.z * b.z - a.w * b.w,
                a.x * b.y + a.y * b.x + a.z * b.w - a.w * b.z,
                a.x * b.z - a.y * b.w + a.z * b.x + a.w * b.y,
                a.x * b.w + a.y * b.z - a.z * b.y + a.w * b.x);
}

[[node]]
void rotate_rotation_global(float4 rotation, float4 rotate_by, out float4 result)
{
  result = quaternion_multiply_float4(rotate_by, rotation);
}

[[node]]
void rotate_rotation_local(float4 rotation, float4 rotate_by, out float4 result)
{
  result = quaternion_multiply_float4(rotation, rotate_by);
}
