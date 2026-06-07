/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_color_utils.glsl"
#include "gpu_shader_math_constants_lib.glsl"

float angle_normalized_v3v3(float3 v1, float3 v2)
{
  v1 = normalize(v1);
  v2 = normalize(v2);
  if (dot(v1, v2) >= 0.0f) {
    return 2.0f * asin(clamp(length(v2 - v1) / 2.0f, -1.0f, 1.0f));
  }
  const float3 v2_n = -v2;
  return M_PI - 2.0f * asin(clamp(length(v2_n - v1) / 2.0f, -1.0f, 1.0f));
}

float component_average(float3 a)
{
  return (a.x + a.y + a.z) / 3.0f;
}

/* Float */

[[node]]
void compare_float_less_than(float a, float b, out float result)
{
  result = float(a < b);
}

[[node]]
void compare_float_less_equal(float a, float b, out float result)
{
  result = float(a <= b);
}

[[node]]
void compare_float_greater_than(float a, float b, out float result)
{
  result = float(a > b);
}

[[node]]
void compare_float_greater_equal(float a, float b, out float result)
{
  result = float(a >= b);
}

[[node]]
void compare_float_equal(float a, float b, float epsilon, out float result)
{
  result = float(abs(a - b) <= epsilon);
}

[[node]]
void compare_float_not_equal(float a, float b, float epsilon, out float result)
{
  result = float(abs(a - b) > epsilon);
}

/* Integer */

[[node]]
void compare_int_less_than(float a, float b, out float result)
{
  result = float(int(a) < int(b));
}

[[node]]
void compare_int_less_equal(float a, float b, out float result)
{
  result = float(int(a) <= int(b));
}

[[node]]
void compare_int_greater_than(float a, float b, out float result)
{
  result = float(int(a) > int(b));
}

[[node]]
void compare_int_greater_equal(float a, float b, out float result)
{
  result = float(int(a) >= int(b));
}

[[node]]
void compare_int_equal(float a, float b, out float result)
{
  result = float(int(a) == int(b));
}

[[node]]
void compare_int_not_equal(float a, float b, out float result)
{
  result = float(int(a) != int(b));
}

/* Vector - Less Than */

[[node]]
void compare_vector_average_less_than(float3 a, float3 b, out float result)
{
  result = float(component_average(a) < component_average(b));
}

[[node]]
void compare_vector_dot_less_than(float3 a, float3 b, float comp, out float result)
{
  result = float(dot(a, b) < comp);
}

[[node]]
void compare_vector_direction_less_than(float3 a, float3 b, float angle, out float result)
{
  result = float(angle_normalized_v3v3(a, b) < angle);
}

[[node]]
void compare_vector_element_less_than(float3 a, float3 b, out float result)
{
  result = float(a.x < b.x && a.y < b.y && a.z < b.z);
}

[[node]]
void compare_vector_length_less_than(float3 a, float3 b, out float result)
{
  result = float(length(a) < length(b));
}

/* Vector - Less Equal */

[[node]]
void compare_vector_average_less_equal(float3 a, float3 b, out float result)
{
  result = float(component_average(a) <= component_average(b));
}

[[node]]
void compare_vector_dot_less_equal(float3 a, float3 b, float comp, out float result)
{
  result = float(dot(a, b) <= comp);
}

[[node]]
void compare_vector_direction_less_equal(float3 a, float3 b, float angle, out float result)
{
  result = float(angle_normalized_v3v3(a, b) <= angle);
}

[[node]]
void compare_vector_element_less_equal(float3 a, float3 b, out float result)
{
  result = float(a.x <= b.x && a.y <= b.y && a.z <= b.z);
}

[[node]]
void compare_vector_length_less_equal(float3 a, float3 b, out float result)
{
  result = float(length(a) <= length(b));
}

/* Vector - Greater Than */

[[node]]
void compare_vector_average_greater_than(float3 a, float3 b, out float result)
{
  result = float(component_average(a) > component_average(b));
}

[[node]]
void compare_vector_dot_greater_than(float3 a, float3 b, float comp, out float result)
{
  result = float(dot(a, b) > comp);
}

[[node]]
void compare_vector_direction_greater_than(float3 a, float3 b, float angle, out float result)
{
  result = float(angle_normalized_v3v3(a, b) > angle);
}

[[node]]
void compare_vector_element_greater_than(float3 a, float3 b, out float result)
{
  result = float(a.x > b.x && a.y > b.y && a.z > b.z);
}

[[node]]
void compare_vector_length_greater_than(float3 a, float3 b, out float result)
{
  result = float(length(a) > length(b));
}

/* Vector - Greater Equal */

[[node]]
void compare_vector_average_greater_equal(float3 a, float3 b, out float result)
{
  result = float(component_average(a) >= component_average(b));
}

[[node]]
void compare_vector_dot_greater_equal(float3 a, float3 b, float comp, out float result)
{
  result = float(dot(a, b) >= comp);
}

[[node]]
void compare_vector_direction_greater_equal(float3 a, float3 b, float angle, out float result)
{
  result = float(angle_normalized_v3v3(a, b) >= angle);
}

[[node]]
void compare_vector_element_greater_equal(float3 a, float3 b, out float result)
{
  result = float(a.x >= b.x && a.y >= b.y && a.z >= b.z);
}

[[node]]
void compare_vector_length_greater_equal(float3 a, float3 b, out float result)
{
  result = float(length(a) >= length(b));
}

/* Vector - Equal */

[[node]]
void compare_vector_average_equal(float3 a, float3 b, float epsilon, out float result)
{
  result = float(abs(component_average(a) - component_average(b)) <= epsilon);
}

[[node]]
void compare_vector_dot_equal(float3 a, float3 b, float comp, float epsilon, out float result)
{
  result = float(abs(dot(a, b) - comp) <= epsilon);
}

[[node]]
void compare_vector_direction_equal(
    float3 a, float3 b, float angle, float epsilon, out float result)
{
  result = float(abs(angle_normalized_v3v3(a, b) - angle) <= epsilon);
}

[[node]]
void compare_vector_element_equal(float3 a, float3 b, float epsilon, out float result)
{
  result = float(abs(a.x - b.x) <= epsilon && abs(a.y - b.y) <= epsilon &&
                 abs(a.z - b.z) <= epsilon);
}

[[node]]
void compare_vector_length_equal(float3 a, float3 b, float epsilon, out float result)
{
  result = float(abs(length(a) - length(b)) <= epsilon);
}

/* Vector - Not Equal */

[[node]]
void compare_vector_average_not_equal(float3 a, float3 b, float epsilon, out float result)
{
  result = float(abs(component_average(a) - component_average(b)) > epsilon);
}

[[node]]
void compare_vector_dot_not_equal(float3 a, float3 b, float comp, float epsilon, out float result)
{
  result = float(abs(dot(a, b) - comp) > epsilon);
}

[[node]]
void compare_vector_direction_not_equal(
    float3 a, float3 b, float angle, float epsilon, out float result)
{
  result = float(abs(angle_normalized_v3v3(a, b) - angle) > epsilon);
}

[[node]]
void compare_vector_element_not_equal(float3 a, float3 b, float epsilon, out float result)
{
  result = float(abs(a.x - b.x) > epsilon || abs(a.y - b.y) > epsilon || abs(a.z - b.z) > epsilon);
}

[[node]]
void compare_vector_length_not_equal(float3 a, float3 b, float epsilon, out float result)
{
  result = float(abs(length(a) - length(b)) > epsilon);
}

/* Color. */

[[node]]
void compare_color_equal(float4 a, float4 b, float epsilon, out float result)
{
  result = float(abs(a.x - b.x) <= epsilon && abs(a.y - b.y) <= epsilon &&
                 abs(a.z - b.z) <= epsilon);
}

[[node]]
void compare_color_not_equal(float4 a, float4 b, float epsilon, out float result)
{
  result = float(abs(a.x - b.x) > epsilon || abs(a.y - b.y) > epsilon || abs(a.z - b.z) > epsilon);
}

[[node]]
void compare_color_brighter(float4 a, float4 b, float3 luminance_coefficients, out float result)
{
  float luminance_a = get_luminance(a.rgb, luminance_coefficients);
  float luminance_b = get_luminance(b.rgb, luminance_coefficients);
  result = float(luminance_a > luminance_b);
}

[[node]]
void compare_color_darker(float4 a, float4 b, float3 luminance_coefficients, out float result)
{
  float luminance_a = get_luminance(a.rgb, luminance_coefficients);
  float luminance_b = get_luminance(b.rgb, luminance_coefficients);
  result = float(luminance_a < luminance_b);
}
