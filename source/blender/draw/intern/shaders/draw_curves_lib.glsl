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

uint curve_id_get(uint point_id)
{
  return point_id / drw_curves.point_per_segment_max;
}

uint point_id_get(uint vertex_id)
{
  uint point = ((vertex_id / drw_curves.vertex_per_segment) + (vertex_id & 1u));
  uint curve = curve_id_get(point);
  uint end_point = texelFetch(curves_offset_buf, int(curve + 1)).r - 1;
  return min(point % drw_curves.point_per_segment_max, end_point);
}

float cylinder_time_get(uint vertex_id)
{
  uint seg_vert_id = vertex_id % drw_curves.vertex_per_segment;
  /* Face count for the visible half cylinder [1..max]. Is 1 for ribbon. */
  uint face_count = drw_curves.half_cylinder_face_count;
  float time = float((seg_vert_id >> 1) & face_count) / float(face_count);
  return time * 2.0f - 1.0f;
}

float3 point_position_get(uint point_id)
{
  return texelFetch(curves_pos_buf, int(point_id)).rgb;
}

float point_radius(uint point_id)
{
  return texelFetch(curves_rad_buf, int(point_id)).r;
}

struct Point {
  /* Position of the evaluated curve point (not the shape / cylinder point). */
  float3 P;
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
  pt.curve_id = curve_id_get(vertex_id);
  pt.point_id = point_id_get(vertex_id);

  uint start_point = texelFetch(curves_offset_buf, int(pt.curve_id)).r;

  pt.P = point_position_get(pt.point_id);
  pt.radius = point_radius(pt.point_id);
  pt.cylinder_time = cylinder_time_get(vertex_id);

  if (pt.point_id == start_point) {
    /* Hair root. */
    pt.T = point_position_get(pt.point_id + 1) - pt.P;
  }
  else {
    pt.T = pt.P - point_position_get(pt.point_id - 1);
  }
  return pt;
}

Point object_to_world(Point pt, float4x4 object_to_world)
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
float3 shape_point_get(Point pt, float3 V, out float3 B)
{
  B = normalize(cross(V, pt.T));
  return pt.P + B * pt.cylinder_time * pt.radius;
}
float3 shape_point_get(Point pt, float3 V)
{
  float3 unused;
  return shape_point_get(pt, V, unused);
}

#  ifdef GPU_VERTEX_SHADER
float get_customdata_float(const samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, int(curve_id_get(point_id_get(gl_VertexID)))).x;
}

float2 get_customdata_vec2(const samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, int(curve_id_get(point_id_get(gl_VertexID)))).xy;
}

float3 get_customdata_vec3(const samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, int(curve_id_get(point_id_get(gl_VertexID)))).xyz;
}

float4 get_customdata_vec4(const samplerBuffer cd_buf)
{
  return texelFetch(cd_buf, int(curve_id_get(point_id_get(gl_VertexID)))).xyzw;
}

float3 get_strand_root_pos()
{
  uint curve_id = curve_id_get(point_id_get(uint(gl_VertexID)));
  uint start_point = texelFetch(curves_offset_buf, int(curve_id)).r;
  return texelFetch(curves_pos_buf, int(start_point)).xyz;
}
#  endif

}  // namespace curves

#endif
