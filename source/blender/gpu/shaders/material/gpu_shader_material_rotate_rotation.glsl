/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_quaternion_lib.glsl"

[[node]]
void rotate_rotation_global(float4 rotation, float4 rotate_by, out float4 result)
{
  result = math_quaternion_multiply(rotate_by, rotation);
}

[[node]]
void rotate_rotation_local(float4 rotation, float4 rotate_by, out float4 result)
{
  result = math_quaternion_multiply(rotation, rotate_by);
}
