/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_math_constants_lib.glsl"
#include "gpu_shader_math_vector_compare_lib.glsl"
#include "gpu_shader_math_rotation_conversion_lib.glsl"
#include "gpu_shader_math_rotation_lib.glsl"

float3 transform_point_by_quaternion(float3 v, float4 q)
{
  const float3 qv = q.yzw;
  const float3 t = 2.0f * cross(qv, v);
  return v + q.x * t + cross(qv, t);
}

float angle_normalized_v3v3(float3 a, float3 b)
{
  const float3 na = normalize(a);
  const float3 nb = normalize(b);
  return acos(clamp(dot(na, nb), -1.0f, 1.0f));
}

float angle_signed_on_axis_v3v3_v3(float3 v1, float3 v2, float3 axis)
{
  const float3 v1_proj = normalize(v1 - dot(v1, axis) * axis);
  const float3 v2_proj = normalize(v2 - dot(v2, axis) * axis);
  float angle = atan(dot(cross(v2_proj, v1_proj), axis), dot(v2_proj, v1_proj));
  if (angle < 0.0f) {
    angle += 2.0f * M_PI;
  }
  return angle;
}

[[node]]
void node_align_rotation_to_vector_auto_pivot(
    float4 old_rotation, float factor, float3 input_vector, float3 local_main_axis, out float4 out_rotation)
{
  if (is_zero(input_vector)) {
    out_rotation = old_rotation;
    return;
  }

  const float3 old_axis = transform_point_by_quaternion(local_main_axis, old_rotation);
  const float3 new_axis = input_vector;

  float3 rotation_axis = cross(old_axis, new_axis);
  if (is_zero(rotation_axis)) {
    /* The vectors are linearly dependent, so we fall back to another axis. */
    rotation_axis = cross(old_axis, float3(1.0f, 0.0f, 0.0f));
    if (is_zero(rotation_axis)) {
      /* This is now guaranteed to not be zero. */
      rotation_axis = cross(old_axis, float3(0.0f, 1.0f, 0.0f));
    }
  }

  const float full_angle = angle_normalized_v3v3(old_axis, new_axis);
  const float angle = factor * full_angle;

  AxisAngle aa;
  aa.axis = normalize(rotation_axis);
  aa.angle = angle;

  out_rotation = math_quaternion_multiply(to_axis_angle(aa).as_float4(), old_rotation);
}

[[node]]
void node_align_rotation_to_vector_fixed_pivot(float4 old_rotation,
                                               float factor,
                                               float3 input_vector,
                                               float3 local_main_axis,
                                               float3 local_pivot_axis,
                                               out float4 out_rotation)
{
  if (local_main_axis == local_pivot_axis) {
    /* Can't compute any meaningful rotation angle in this case. */
    out_rotation = old_rotation;
    return;
  }
  if (is_zero(input_vector)) {
    out_rotation = old_rotation;
    return;
  }

  const float3 old_axis = transform_point_by_quaternion(local_main_axis, old_rotation);
  const float3 pivot_axis = transform_point_by_quaternion(local_pivot_axis, old_rotation);

  float full_angle = angle_signed_on_axis_v3v3_v3(input_vector, old_axis, pivot_axis);
  if (full_angle > M_PI) {
    /* Make sure the point is rotated as little as possible. */
    full_angle -= 2.0f * M_PI;
  }

  const float angle = factor * full_angle;

  AxisAngle aa;
  aa.axis = normalize(pivot_axis);
  aa.angle = angle;

  out_rotation = math_quaternion_multiply(to_axis_angle(aa).as_float4(), old_rotation);
}
