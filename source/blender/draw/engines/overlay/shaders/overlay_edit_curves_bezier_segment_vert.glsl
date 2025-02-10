/* SPDX-FileCopyrightText: 2018-2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "common_view_clipping_lib.glsl"
#include "draw_model_lib.glsl"
#include "draw_view_lib.glsl"
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
  const uint segment_i = gpu_index_load(vertex_id);
  const uvec2 start_point_indices = gpu_attr_load_int2(point_index, gpu_attr_0, segment_i);
  const uvec2 end_point_indices = gpu_attr_load_int2(point_index, gpu_attr_0, segment_i + 1);
  const int left_handle_offset = curves_data.point_num;
  const int right_handle_offset = left_handle_offset + curves_data.bezier_point_num;

  VertIn vert_in;
  vert_in.p[0] = gpu_attr_load_float3(pos, gpu_attr_3, start_point_indices.x);
  vert_in.p[1] = gpu_attr_load_float3(pos, gpu_attr_3, start_point_indices.y + right_handle_offset);
  vert_in.p[2] = gpu_attr_load_float3(pos, gpu_attr_3, end_point_indices.y + left_handle_offset);
  vert_in.p[3] = gpu_attr_load_float3(pos, gpu_attr_3, end_point_indices.x);
  vert_in.first_vertex_id = evaluated_points_offset[gpu_attr_load_index(segment_i, gpu_attr_1)];
  vert_in.resolution = evaluated_points_offset[gpu_attr_load_index(segment_i + 1, gpu_attr_1)] -
                       vert_in.first_vertex_id;
  vert_in.radius = vec2(radius[gpu_attr_load_index(segment_i, gpu_attr_2)],
                        radius[gpu_attr_load_index(segment_i + 1, gpu_attr_2)]);
  return vert_in;
}

vec2 circle_tangent(vec2 center_a, float radius_a, vec2 center_b, float radius_b, bool first)
{
  const vec2 delta = center_b - center_a;
  const float delta_r = radius_a - radius_b;
  const float squared = dot(delta, delta);
  if (squared < delta_r * delta_r) {
    return vec2();
  }
  const float dist = sqrt(squared);
  const float gamma = atan2(delta.y, delta.x);
  const float theta = acos(delta_r / dist);

  const float alpha = gamma + (first ? theta : -theta);
  return radius_a * vec2(cos(alpha), sin(alpha));
}

vec2 radius_offset(vec3 curve_point,
                   vec4 ndc_curve_point,
                   float u,
                   float u2,
                   inout vec3 points[4],
                   vec2 radii,
                   bool first_tangent)
{
  const vec3 curve_point2 = calc_bezier_point(u2, points);
  const vec4 ndc_curve_point2 = drw_point_object_to_homogenous(curve_point2);
  const float radius = radius_to_ndc(mix(radii.x, radii.y, u) * sizeViewport.x,
                                     drw_point_object_to_view(curve_point));
  const float radius2 = radius_to_ndc(mix(radii.x, radii.y, u2) * sizeViewport.x,
                                      drw_point_object_to_view(curve_point2));

  return circle_tangent((ndc_curve_point.xy / ndc_curve_point.w) * sizeViewport,
                        radius,
                        (ndc_curve_point2.xy / ndc_curve_point2.w) * sizeViewport,
                        radius2,
                        first_tangent);
}

vec2 tangent_offset(vec3 curve_point, vec4 ndc_curve_point, vec3 tangent)
{
  const vec4 ndc_tangent = drw_point_object_to_homogenous(curve_point + tangent);
  const vec2 tangent_2d = normalize(ndc_tangent.xy * ndc_curve_point.w -
                                    ndc_curve_point.xy * ndc_tangent.w);
  return vec2(-tangent_2d.y, tangent_2d.x) * sizeEdge * 2 * (gl_VertexID % 2 ? -1.0 : 1.0);
}

vec3 calc_bezier_point(float u, inout vec3 control_points[4])
{
  control_points[0] += (control_points[1] - control_points[0]) * u;
  control_points[1] += (control_points[2] - control_points[1]) * u;
  control_points[2] += (control_points[3] - control_points[2]) * u;

  control_points[0] += (control_points[1] - control_points[0]) * u;
  control_points[1] += (control_points[2] - control_points[1]) * u;

  return control_points[0] + (control_points[1] - control_points[0]) * u;
}

float radius_to_ndc(float radius, vec3 view_point)
{
  const vec3 view_radius = vec3(radius, 0.0, view_point.z);
  const vec4 ndc_radius = drw_point_view_to_homogenous(view_radius);
  return ndc_radius.x / ndc_radius.w;
}

void main()
{
  const int vertex_per_quad = 6;
  VertIn vert_in = input_assembly(gl_VertexID / vertex_per_quad);
#ifdef SEGMENT
  const int segment_vertex_i = gl_VertexID - vert_in.first_vertex_id * vertex_per_quad;
#else
  const int segment_vertex_i = gl_VertexID -
                               (endpointsOnly ? 0 : vert_in.first_vertex_id * vertex_per_quad);
#endif
  const int quad_i = segment_vertex_i / vertex_per_quad;
  const int in_quad_i = segment_vertex_i % vertex_per_quad;
  const bool quad_right = in_quad_i >= 2 && in_quad_i != 5;
  const bool bottom_edge = in_quad_i % 2;
#ifdef SEGMENT
  vec3 points[4] = float4_array(vert_in.p[0], vert_in.p[1], vert_in.p[2], vert_in.p[3]);
  const int step = quad_i + quad_right;
  const float u = float(step) / vert_in.resolution;
  const vec3 curve_point = calc_bezier_point(u, vert_in.p);
#else
  vec3 curve_point;
  float u = 0;
  if (endpointsOnly) {
    curve_point = vert_in.p[0];
  }
  else {
    const int step = quad_i;
    u = float(step) / vert_in.resolution;
    curve_point = calc_bezier_point(u, vert_in.p);
  }
#endif

  const vec3 world_pos = drw_point_object_to_world(curve_point);
  vec4 ndc_pos = drw_point_world_to_homogenous(world_pos);

#ifdef SEGMENT
  const vec2 offset = displayRadius ?
                          radius_offset(curve_point,
                                        ndc_pos,
                                        u,
                                        float(quad_right ? step - 1 : step + 1) /
                                            vert_in.resolution,
                                        points,
                                        vert_in.radius,
                                        quad_right ? bottom_edge : !bottom_edge) :
                          tangent_offset(curve_point, ndc_pos, vert_in.p[1] - vert_in.p[0]);
#else
  const float x = quad_right ? 1.0 : -1.0;
  const float y = bottom_edge ? -1.0 : 1.0;
  const float radius = radius_to_ndc(mix(vert_in.radius.x, vert_in.radius.y, u) * sizeViewport.x,
                                     drw_point_world_to_view(world_pos));
  const vec2 offset = vec2(x, y) * radius;

  uv_coord = vec2(x, y);
#endif

  ndc_pos.xy += offset * ndc_pos.w * sizeViewportInv;
  gl_Position = ndc_pos;
  finalColor = colorWireEdit;
  view_clipping_distances(world_pos);
}
