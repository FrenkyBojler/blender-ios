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
};

VertIn input_assembly(uint vertex_id)
{
  uint segment_i = gpu_index_load(vertex_id);
  ivec4 indices = gpu_attr_load_int4(segments, gpu_attr_0, segment_i);

  VertIn vert_in;
  vert_in.p[0] = gpu_attr_load_float3(pos, gpu_attr_3, indices.x);
  vert_in.p[1] = gpu_attr_load_float3(pos, gpu_attr_3, indices.y);
  vert_in.p[2] = gpu_attr_load_float3(pos, gpu_attr_3, indices.z);
  vert_in.p[3] = gpu_attr_load_float3(pos, gpu_attr_3, indices.w);
  vert_in.first_vertex_id = first_id[gpu_attr_load_index(segment_i, gpu_attr_1)];
  vert_in.resolution = resolution[gpu_attr_load_index(segment_i, gpu_attr_2)];
  return vert_in;
}


void main()
{
  VertIn vert_in = input_assembly(gl_VertexID / 3);
  int segment_vertex_i = gl_VertexID - vert_in.first_vertex_id * 3;
  int quad_i = segment_vertex_i / 6;
  int in_quad_i = segment_vertex_i % 6;

  int step = quad_i + ((in_quad_i < 2 || in_quad_i == 3) ? 0 : 1);
  float u = float(step) / vert_in.resolution;

  vec3 q[4] = float3_array(
    point_object_to_world(vert_in.p[0]),
    point_object_to_world(vert_in.p[1]), 
    point_object_to_world(vert_in.p[2]), 
    point_object_to_world(vert_in.p[3]));

  q[0] += (q[1] - q[0]) * u;
  q[1] += (q[2] - q[1]) * u;
  q[2] += (q[3] - q[2]) * u;

  q[0] += (q[1] - q[0]) * u;
  q[1] += (q[2] - q[1]) * u;

  vec4 c0 = point_world_to_ndc(q[0]);
  vec4 c1 = point_world_to_ndc(q[1]);
  vec2 tangent = c1.xy / c1.w - c0.xy / c0.w;
  
  q[0] += (q[1] - q[0]) * u;

  vec4 ndc_pos = point_world_to_ndc(q[0]);

  vec2 normal = normalize(vec2(-tangent.y, tangent.x)) * 2 * sizeViewportInv;
  normal *= gl_VertexID % 2 ? -1.0 : 1.0;
  ndc_pos.xy += normal * ndc_pos.w;
  gl_Position = ndc_pos;
  view_clipping_distances(q[0]);
  finalColor = vec4(0.0, 0.0, 0.0, 1.0);
}
