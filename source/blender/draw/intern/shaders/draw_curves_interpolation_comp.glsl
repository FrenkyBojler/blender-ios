/* SPDX-FileCopyrightText: 2021-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * GPU generated interpolated position and radius. Updated on attribute change.
 * One thread processes one curve.
 *
 * Equivalent of `CurvesGeometry::evaluated_positions()`.
 */

#include "draw_curves_info.hh"

COMPUTE_SHADER_CREATE_INFO(draw_curves_interpolation)

#include "gpu_shader_attribute_load_lib.glsl"

struct IndexRange {
  int start;
  int size;

  METAL_CONSTRUCTOR_2(IndexRange, int, start, int, size)
};

int size(const IndexRange range)
{
  return range.size;
}

IndexRange from_begin_end(int begin, int end)
{
  return IndexRange(begin, end - begin);
}

/* Returns a.slice(b). */
IndexRange slice(IndexRange a, IndexRange b)
{
  return IndexRange(a.start + b.start, b.size);
}

/* Copy of DNA enum in `DNA_curves_types.h`. */
enum CurveType : uint32_t {
  CURVE_TYPE_CATMULL_ROM = 0u,
  CURVE_TYPE_POLY = 1u,
  CURVE_TYPE_BEZIER = 2u,
  CURVE_TYPE_NURBS = 3u,
};

template<typename DataT> DataT mix4(DataT v0, DataT v1, DataT v2, DataT v3, float4 w)
{
  return v0 * w.x + v1 * w.y + v2 * w.z + v3 * w.w;
}

template float mix4<float>(float, float, float, float, float4);
template float2 mix4<float2>(float2, float2, float2, float2, float4);
template float3 mix4<float3>(float3, float3, float3, float3, float4);
template float4 mix4<float4>(float4, float4, float4, float4, float4);

struct EvaluatedPoint {
  float3 position;
  float radius;
};

namespace catmull_rom {

float4 calculate_basis(const float parameter)
{
  /* Adapted from Cycles #catmull_rom_basis_eval function. */
  const float t = parameter;
  const float s = 1.0f - parameter;
  return 0.5f * float4(-t * s * s,
                       2.0f + t * t * (3.0f * t - 5.0f),
                       2.0f + s * s * (3.0f * s - 5.0f),
                       -s * t * t);
}

int4 get_points(uint segment_id, IndexRange curve_range)
{
  int4 point_ids = int(segment_id) + int4(-1, +0, +1, +2);
  return clamp(int(curve_range.start) + point_ids,
               int4(curve_range.start),
               int4(curve_range.start + curve_range.size - 1));
}

float4 get_weights(float parameter)
{
  return catmull_rom::calculate_basis(parameter);
}

EvaluatedPoint get_evaluated_point(uint segment_id, IndexRange curve_range, float parameter)
{
  const int4 point_ids = get_points(segment_id, curve_range);
  const float3 lP_0 = gpu_attr_load_float3(points_pos_buf, int2(3, 0), point_ids.x);
  const float3 lP_1 = gpu_attr_load_float3(points_pos_buf, int2(3, 0), point_ids.y);
  const float3 lP_2 = gpu_attr_load_float3(points_pos_buf, int2(3, 0), point_ids.z);
  const float3 lP_3 = gpu_attr_load_float3(points_pos_buf, int2(3, 0), point_ids.w);

  const float rad_0 = points_rad_buf[point_ids.x];
  const float rad_1 = points_rad_buf[point_ids.y];
  const float rad_2 = points_rad_buf[point_ids.z];
  const float rad_3 = points_rad_buf[point_ids.w];

  EvaluatedPoint pt;
  const float4 weights = get_weights(parameter);
  pt.position = mix4(lP_0, lP_1, lP_2, lP_3, weights);
  pt.radius = mix4(rad_0, rad_1, rad_2, rad_3, weights);
  return pt;
}

void evaluate_curve(IndexRange curve_range, IndexRange evaluated_range, uint curve_resolution)
{
  for (uint i = 0; i < evaluated_range.size; i++) {
    const uint out_id = evaluated_range.start + i;
    const uint segment_id = i / curve_resolution;
    const float parameter = float(i % curve_resolution) / float(curve_resolution);

    EvaluatedPoint pt = get_evaluated_point(segment_id, curve_range, parameter);
    points_pos_rad_buf[out_id] = float4(pt.position, pt.radius);
  }
}

}  // namespace catmull_rom

namespace bezier {

IndexRange per_curve_point_offsets_range(const IndexRange points, const int curve_index)
{
  return IndexRange(curve_index + points.start, points.size + 1);
}

int2 get_points(uint segment_id, IndexRange curve_range)
{
  int2 point_ids = int(segment_id) + int2(+0, +1);
  return clamp(int(curve_range.start) + point_ids,
               int2(curve_range.start),
               int2(curve_range.start + curve_range.size - 1));
}

void evaluate_segment_positions(const float3 point_0,
                                const float3 point_1,
                                const float3 point_2,
                                const float3 point_3,
                                const float radius_0,
                                const float radius_3,
                                const IndexRange result)
{
  assert(result.size > 0);
  const float inv_len = 1.0f / float(result.size);
  const float inv_len_squared = inv_len * inv_len;
  const float inv_len_cubed = inv_len_squared * inv_len;

  const float3 rt1 = 3.0f * (point_1 - point_0) * inv_len;
  const float3 rt2 = 3.0f * (point_0 - 2.0f * point_1 + point_2) * inv_len_squared;
  const float3 rt3 = (point_3 - point_0 + 3.0f * (point_1 - point_2)) * inv_len_cubed;

  float3 q0 = point_0;
  float3 q1 = rt1 + rt2 + rt3;
  float3 q2 = 2.0f * rt2 + 6.0f * rt3;
  float3 q3 = 6.0f * rt3;
  for (int i = 0; i < result.size; i++) {
    /* Radius is like any other point attribute and is linearly interpolated. */
    const float rad = mix(radius_0, radius_3, float(i) * inv_len);

    points_pos_rad_buf[result.start + i] = float4(q0, rad);
    q0 += q1;
    q1 += q2;
    q2 += q3;
  }
}

void evaluate_curve(const IndexRange curve_range,
                    const IndexRange evaluated_range,
                    const int curve_id)
{
  /* Range used for indexing bezier offsets. */
  const IndexRange offsets = per_curve_point_offsets_range(curve_range, curve_id);

  for (int i = 0; i < curve_range.size; i++) {
    /* Bezier curves can have different number of evaluated segment per curve segment. */
    const IndexRange segment_range = from_begin_end(bezier_offsets_buf[offsets.start + i],
                                                    bezier_offsets_buf[offsets.start + i + 1]);

    const IndexRange evaluated_segment_range = slice(evaluated_range, segment_range);

    const int2 point_ids = get_points(i, curve_range);

    const float3 lP_0 = gpu_attr_load_float3(points_pos_buf, int2(3, 0), point_ids.x);
    const float3 lP_1 = gpu_attr_load_float3(handles_pos_right_buf, int2(3, 0), point_ids.x);
    const float3 lP_2 = gpu_attr_load_float3(handles_pos_left_buf, int2(3, 0), point_ids.y);
    const float3 lP_3 = gpu_attr_load_float3(points_pos_buf, int2(3, 0), point_ids.y);

    const float rad_0 = points_rad_buf[point_ids.x];
    const float rad_1 = points_rad_buf[point_ids.y];

    evaluate_segment_positions(lP_0, lP_1, lP_2, lP_3, rad_0, rad_1, evaluated_segment_range);
  }
}

}  // namespace bezier

void main()
{
  int curve_id = int(gl_GlobalInvocationID.x);
  if (curve_id >= curves_count) {
    return;
  }

  IndexRange curve_range = from_begin_end(curves_offsets_buf[curve_id],
                                          curves_offsets_buf[curve_id + 1]);

  IndexRange evaluated_range = from_begin_end(curves_evaluated_offsets_buf[curve_id],
                                              curves_evaluated_offsets_buf[curve_id + 1]);

  const uint curve_resolution = curves_resolution_buf[curve_id];
  const CurveType curve_type = CurveType(curves_type_buf[curve_id]);

  switch (curve_type) {
    case CURVE_TYPE_CATMULL_ROM:
      catmull_rom::evaluate_curve(curve_range, evaluated_range, curve_resolution);
      break;
    case CURVE_TYPE_BEZIER:
      bezier::evaluate_curve(curve_range, evaluated_range, curve_id);
      break;
    case CURVE_TYPE_POLY:
    case CURVE_TYPE_NURBS:
      /* Not implemented. */
      break;
  }

  if (true /* TODO(fclem) Make it optional. */) {
    float distance_along_curve = 0.0f;
    points_time_buf[0] = 0.0f;
    for (int i = 1; i < evaluated_range.size; i++) {
      int p = evaluated_range.start + i;
      distance_along_curve += distance(points_pos_rad_buf[p].xyz, points_pos_rad_buf[p - 1].xyz);
      points_time_buf[p] = distance_along_curve;
    }
    curves_length_buf[curve_id] = distance_along_curve;
    for (int i = 1; i < evaluated_range.size; i++) {
      int p = evaluated_range.start + i;
      points_time_buf[p] /= distance_along_curve;
    }
  }
}
