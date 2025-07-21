/* SPDX-FileCopyrightText: 2018-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_object_infos_info.hh"

/**
 * Library to create hairs dynamically from control points.
 * This is less bandwidth intensive than fetching the vertex attributes
 * but does more ALU work per vertex. This also reduces the amount
 * of data the CPU has to precompute and transfer for each update.
 */

#include "gpu_shader_math_matrix_lib.glsl"

/* Avoid including hair functionality for shaders and materials which do not require hair.
 * Required to prevent compilation failure for missing shader inputs and uniforms when hair library
 * is included via other libraries. These are only specified in the ShaderCreateInfo when needed.
 */
#ifdef CURVES_SHADER
#  ifndef DRW_HAIR_INFO
#    error Ensure createInfo includes draw_hair.
#  endif

SHADER_LIBRARY_CREATE_INFO(draw_curves_infos)
SHADER_LIBRARY_CREATE_INFO(draw_curves)

namespace curves {

uint point_id(uint vertex_id)
{
  return (vertex_id / drw_curves.vertex_per_segment) + (vertex_id & 1);
}

float cylinder_time_get(uint vertex_id)
{
  uint seg_vert_id = vertex_id % drw_curves.vertex_per_segment;
  /* Face count for the visible half cylinder [1..max]. Is 1 for ribbon. */
  uint face_count = drw_curves.half_cylinder_face_count;
  float time = float((seg_vert_id >> 1) & face_count) / float(face_count);
  return time * 2.0f - 1.0f;
}

float3 point_position(uint point_id)
{
  return texelFetch(curves_pos_buf, point_id).rgb;
}

float point_time(uint point_id)
{
  return texelFetch(curves_time_buf, point_id).r;
}

float point_radius(uint point_id)
{
  return texelFetch(curves_rad_buf, point_id).r;
}

uint curve_id(uint point_id)
{
  return texelFetch(curves_curve_id_buf, point_id).r;
}

struct Point {
  /* Position of the evaluated curve point (not the shape / cylinder point). */
  float3 P;
  float time;
  /* Tangent vector going from the root to the tip of the curve. */
  float3 T;
  float radius;
  /* Where the vertex is placed on the cross section of the segment cylinder. Range [-1..1]. */
  float cylinder_time;

  uint point_id;
  uint curve_id;
};

/* Return data about the curve point. */
Point point_get(uint vertex_id)
{
  Point pt;
  pt.point_id = point_id(vertex_id);
  pt.curve_id = curve_id(vertex_id);

  pt.P = point_position(pt.point_id);
  pt.time = point_time(pt.point_id);
  pt.radius = point_radius(pt.point_id);
  pt.cylinder_time = cylinder_time_get(vertex_id);

  if (pt.time == 0.0f) {
    /* Hair root. */
    pt.T = point_position(pt.point_id + 1) - pt.P;
  }
  else {
    pt.T = pt.P - point_position(pt.point_id - 1);
  }
  return pt;
}

curves::Point object_to_world(curves::Point pt, float4x4 object_to_world)
{
  pt.P = transform_point(object_to_world, pt.P);
  pt.T = normalize(transform_direction(object_to_world, pt.T));
  pt.radius *= length(to_scale(object_to_world));
  return pt;
}

/**
 * Return the position of the expanded position in world-space.
 * \arg pt : world space curve point.
 * \arg V : world space view vector (toward viewer) at `pt.P`.
 */
float3 shape_point_get(curves::Point pt, float3 V, out float3 B)
{
  B = normalize(cross(V, pt.T));
  return pt.P + B * pt.cylinder_time * pt.radius;
}
float3 shape_point_get(curves::Point pt, float3 V)
{
  float3 unused;
  return shape_point_get(pt, V, unused);
}

float get_customdata_float(const samplerBuffer cd_buf)
{
  /* TODO(fclem) */
  return 0.0;
}

float2 get_customdata_vec2(const samplerBuffer cd_buf)
{
  /* TODO(fclem) */
  return float2(0.0);
}

float3 get_customdata_vec3(const samplerBuffer cd_buf)
{
  /* TODO(fclem) */
  return float3(0.0);
}

float4 get_customdata_vec4(const samplerBuffer cd_buf)
{
  /* TODO(fclem) */
  return float4(0.0);
}

float3 get_strand_pos()
{
  /* TODO(fclem) */
  return float3(0.0);
}

}  // namespace curves

#endif
