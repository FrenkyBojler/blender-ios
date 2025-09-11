/* SPDX-FileCopyrightText: 2016-2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_xr_raycast_info.hh"

VERTEX_SHADER_CREATE_INFO(gpu_shader_xr_raycast)

vec3 catmull_rom(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t)
{
  float t2 = t * t;
  float t3 = t2 * t;
  return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

vec3 get_control_point(int idx)
{
  idx = clamp(idx, 0, control_point_count - 1);
  return control_points[idx].xyz;
}

void main()
{
  int sample_idx = gl_VertexID >> 1;
  float side = ((gl_VertexID & 1) != 0) ? -1.0 : 1.0;

  int segment_idx = sample_idx / samples_per_segment;
  int sample_in_segment = sample_idx - segment_idx * samples_per_segment;
  float t = float(sample_in_segment) / float(samples_per_segment - 1);

  segment_idx = clamp(segment_idx, 0, max(0, control_point_count - 2));

  vec3 p0 = get_control_point(segment_idx - 1);
  vec3 p1 = get_control_point(segment_idx + 0);
  vec3 p2 = get_control_point(segment_idx + 1);
  vec3 p3 = get_control_point(segment_idx + 2);

  vec3 pos = catmull_rom(p0, p1, p2, p3, t) + 0.5 * width * side * right_vector;

  gl_Position = ModelViewProjectionMatrix * vec4(pos, 1.0);
}
