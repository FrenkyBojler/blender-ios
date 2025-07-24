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

/* Indirection buffer indexing. */
int segment_id_get(uint vertex_id)
{
  uint id = vertex_id / drw_curves.vertex_per_segment;
  const bool is_cylinder = drw_curves.half_cylinder_face_count > 1u;
  if (is_cylinder) {
    id += vertex_id & 1u;
  }
  return int(id);
}

#  define END_OF_CURVE 0x7FFFFFFF

struct Indirection {
  /* Can be equal to END_OF_CURVE with ribbon draw type. */
  int curve_id;
  /* Segment ID starting at 0 at curve start. */
  int curve_segment;
};

Indirection indirection_get(int segment_id)
{
  Indirection res;
  res.curve_id = texelFetch(curves_indirection_buf, segment_id).r;

  if (res.curve_id > 0) {
    /* This is start or end of curve. */
    res.curve_segment = 0;
  }
  else {
    res.curve_segment = -res.curve_id;
    res.curve_id = texelFetch(curves_indirection_buf, segment_id + res.curve_id).r;
  }
  return res;
}

int point_id_get(int segment_id, int curve_id)
{
  const bool is_ribbon = drw_curves.half_cylinder_face_count == 1u;
  return segment_id + (is_ribbon ? -curve_id : curve_id);
}

float azimuthal_offset_get(uint vertex_id)
{
  float time;
  const bool is_cylinder = drw_curves.half_cylinder_face_count > 1u;
  if (is_cylinder) {
    uint seg_vert_id = vertex_id % drw_curves.vertex_per_segment;
    /* Face count for the visible half cylinder [1..max]. Is 1 for ribbon. */
    uint face_count = drw_curves.half_cylinder_face_count;
    time = float((seg_vert_id >> 1) & face_count) / float(face_count);
  }
  else {
    time = vertex_id & 1u;
  }
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
  /* Lateral/Azimuthal offset from the center of the curve's width. Range [-1..1]. */
  float azimuthal_offset;

  int point_id;
  int curve_id;
  int curve_segment;
};

/* Return data about the curve point. */
Point point_get(uint vertex_id)
{
  int segment_id = segment_id_get(vertex_id);
  Indirection indirection = indirection_get(segment_id);

  Point pt;
  pt.curve_id = indirection.curve_id;
  pt.curve_segment = indirection.curve_segment;
  pt.point_id = point_id_get(segment_id, indirection.curve_id);

  pt.P = (pt.curve_id == END_OF_CURVE) ? float3(NAN_FLT) : point_position_get(pt.point_id);
  pt.radius = point_radius(pt.point_id);
  pt.azimuthal_offset = azimuthal_offset_get(vertex_id);

  if (pt.curve_segment == 0) {
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
  return pt.P + B * pt.azimuthal_offset * pt.radius;
}
float3 shape_point_get(Point pt, float3 V)
{
  float3 unused;
  return shape_point_get(pt, V, unused);
}

#  ifdef GPU_VERTEX_SHADER
float get_customdata_float(const samplerBuffer cd_buf)
{
  /* TODO(fclem): Pass curve_id to this function. */
  return texelFetch(cd_buf, int(0)).x;
}

float2 get_customdata_vec2(const samplerBuffer cd_buf)
{
  /* TODO(fclem): Pass curve_id to this function. */
  return texelFetch(cd_buf, int(0)).xy;
}

float3 get_customdata_vec3(const samplerBuffer cd_buf)
{
  /* TODO(fclem): Pass curve_id to this function. */
  return texelFetch(cd_buf, int(0)).xyz;
}

float4 get_customdata_vec4(const samplerBuffer cd_buf)
{
  /* TODO(fclem): Pass curve_id to this function. */
  return texelFetch(cd_buf, int(0)).xyzw;
}

float3 get_strand_root_pos()
{
  /* TODO(fclem): Pass point_id and curve_segment to this function. */
  uint point_id = 0;
  uint curve_segment = 0;
  uint start_point = point_id - curve_segment;
  return texelFetch(curves_pos_buf, int(start_point)).xyz;
}
#  endif

}  // namespace curves

#endif
