/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_rotation_conversion_lib.glsl"

float3 get_orthogonal_vector(float3 v)
{
  if (v.x != -v.y) {
    return float3(-v.y, v.x, 0.0);
  }
  if (v.x != -v.z) {
    return float3(-v.z, 0.0, v.x);
  }
  return float3(0.0, -v.z, v.y);
}

[[node]]
void node_axes_to_rotation(float3 primary_in,
                           float3 secondary_in,
                           float primary_idx_f,
                           float secondary_idx_f,
                           float tertiary_idx_f,
                           float tertiary_factor,
                           out float4 out_rot)
{
  float3 primary = primary_in;
  float3 secondary = secondary_in;
  float3 tertiary;

  bool primary_is_non_zero = (length(primary) > 0.0);
  bool secondary_is_non_zero = (length(secondary) > 0.0);

  if (primary_is_non_zero && secondary_is_non_zero) {
    primary = normalize(primary);
    tertiary = cross(primary, secondary);
    if (length(tertiary) == 0.0) {
      tertiary = get_orthogonal_vector(primary);
    }
    tertiary = normalize(tertiary);
    secondary = cross(tertiary, primary);
  }
  else if (primary_is_non_zero) {
    primary = normalize(primary);
    secondary = get_orthogonal_vector(primary);
    secondary = normalize(secondary);
    tertiary = cross(primary, secondary);
  }
  else if (secondary_is_non_zero) {
    secondary = normalize(secondary);
    primary = get_orthogonal_vector(secondary);
    primary = normalize(primary);
    tertiary = cross(primary, secondary);
  }
  else {
    out_rot = float4(1.0, 0.0, 0.0, 0.0);
    return;
  }

  int primary_axis = int(primary_idx_f);
  int secondary_axis = int(secondary_idx_f);
  int tertiary_axis = int(tertiary_idx_f);

  float3x3 mat;
  mat[primary_axis] = primary;
  mat[secondary_axis] = secondary;
  mat[tertiary_axis] = tertiary_factor * tertiary;

  out_rot = to_quaternion(mat).as_float4();
}

[[node]]
void node_axes_to_rotation_identity(float3 primary_in,
                                    float3 secondary_in,
                                    out float4 out_rot)
{
  out_rot = float4(1.0, 0.0, 0.0, 0.0);
}
