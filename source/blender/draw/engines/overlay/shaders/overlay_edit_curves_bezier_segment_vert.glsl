/* SPDX-FileCopyrightText: 2018-2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "common_view_clipping_lib.glsl"
#include "common_view_lib.glsl"
#include "gpu_shader_attribute_load_lib.glsl"
#include "gpu_shader_index_load_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"


struct VertIn {
  vec3 p[4];
  int first_vertex_id;
  int resolution;
  vec2 radius;
};

VertIn input_assembly(uint vertex_id)
{
  uint segment_i = gpu_index_load(vertex_id);
  ivec4 indices = gpu_attr_load_int4(segments, gpu_attr_0, segment_i);

  VertIn vert_in;
  vert_in.p[0] = gpu_attr_load_float3(pos, gpu_attr_4, indices.x);
  vert_in.p[1] = gpu_attr_load_float3(pos, gpu_attr_4, indices.y);
  vert_in.p[2] = gpu_attr_load_float3(pos, gpu_attr_4, indices.z);
  vert_in.p[3] = gpu_attr_load_float3(pos, gpu_attr_4, indices.w);
  vert_in.first_vertex_id = first_id[gpu_attr_load_index(segment_i, gpu_attr_1)];
  vert_in.resolution = resolution[gpu_attr_load_index(segment_i, gpu_attr_2)];
  vert_in.radius = gpu_attr_load_float2(radius, gpu_attr_3, segment_i);
  return vert_in;
}

void calc_bezier_point(float u, in vec3 control_points[4], out vec3 curve_point, out vec3 tangent) {
  control_points[0] += (control_points[1] - control_points[0]) * u;
  control_points[1] += (control_points[2] - control_points[1]) * u;
  control_points[2] += (control_points[3] - control_points[2]) * u;

  control_points[0] += (control_points[1] - control_points[0]) * u;
  control_points[1] += (control_points[2] - control_points[1]) * u;
  
  tangent = control_points[1] - control_points[0];
  
  control_points[0] += (control_points[1] - control_points[0]) * u;
  curve_point = control_points[0];
  tangent += curve_point;
}

void main()
{
  const int vertex_per_quad = 6;
  VertIn vert_in = input_assembly(gl_VertexID / vertex_per_quad);
  int segment_vertex_i = gl_VertexID - vert_in.first_vertex_id * vertex_per_quad;
  int quad_i = segment_vertex_i / vertex_per_quad;
  int in_quad_i = segment_vertex_i % vertex_per_quad;

  int step = quad_i + ((in_quad_i < 2 || in_quad_i == 3) ? 0 : 1);
  float u = float(step) / vert_in.resolution;

  vec3 curve_point;
  vec3 tangent;
  calc_bezier_point(u, vert_in.p, curve_point, tangent);
  
  vec3 world_pos = point_object_to_world(curve_point);
  vec4 ndc_pos = point_world_to_ndc(world_pos);
  vec4 ndc_tan = point_object_to_ndc(tangent);

  float radius = mix(vert_in.radius.x, vert_in.radius.y, u) * 5;
  vec3 view_radius = vec3(radius, 0.0, point_world_to_view(world_pos).z);
  vec4 ndc_radius = point_view_to_ndc(view_radius);
  float normal_size = ndc_radius.x / ndc_radius.w;

  vec2 tangent2d = ndc_tan.xy / ndc_tan.w - ndc_pos.xy / ndc_pos.w;
  vec2 normal = normalize(vec2(-tangent2d.y, tangent2d.x)) * normal_size * 2 * sizeViewportInv;
  normal *= gl_VertexID % 2 ? -1.0 : 1.0;
  ndc_pos.xy += normal * ndc_pos.w;
  gl_Position = ndc_pos;
  view_clipping_distances(world_pos);
  finalColor = vec4(0.0, 0.0, 0.0, 1.0);
}
