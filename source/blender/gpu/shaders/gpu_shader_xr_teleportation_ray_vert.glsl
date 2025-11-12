/* SPDX-FileCopyrightText: 2016-2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_xr_teleportation_ray_infos.hh"

VERTEX_SHADER_CREATE_INFO(gpu_shader_xr_teleportation_ray)

vec3 catmull_rom(vec3 p0, vec3 p1, vec3 p2, vec3 p3, float t)
{
  float t2 = t * t;
  float t3 = t2 * t;
  return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
                (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

vec3 get_control_point(int idx)
{
  idx = clamp(idx, 0, endpoint_idx);
  return control_points[idx].xyz;
}

void main()
{
  int sample_idx = gl_VertexID >> 1;
  float side = ((gl_VertexID & 1) != 0) ? -1.0 : 1.0;

  /** Interpolate within the range: [0, segment_count] */
  float sample_count = XR_TELEPORTATION_ARC_SAMPLES;
  float sample_value = (float(sample_idx) * float(endpoint_idx)) / float(sample_count);

  int segment_idx = int(sample_value);
  float t = sample_value - float(segment_idx);

  vec3 p0 = get_control_point(segment_idx - 1);
  vec3 p1 = get_control_point(segment_idx + 0);
  vec3 p2 = get_control_point(segment_idx + 1);
  vec3 p3 = get_control_point(segment_idx + 2);

  vec3 pos = catmull_rom(p0, p1, p2, p3, t) + 0.5 * line_width * side * right_vector;

  gl_Position = ModelViewProjectionMatrix * vec4(pos, 1.0);
}
