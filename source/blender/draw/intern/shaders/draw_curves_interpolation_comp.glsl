/* SPDX-FileCopyrightText: 2021-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * GPU generated indirection buffer. Updated on attribute change.
 * One thread processes one curve.
 */

#include "draw_curves_info.hh"

COMPUTE_SHADER_CREATE_INFO(draw_curves_interpolation)

#include "gpu_shader_attribute_load_lib.glsl"

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

int4 get_points(uint segment_id, uint curve_start, uint curve_end)
{
  int4 point_ids = int(segment_id) + int4(-1, +0, +1, +2);
  return clamp(int(curve_start) + point_ids, int4(curve_start), int4(curve_end - 1));
}

float4 get_weights(float parameter)
{
  return catmull_rom::calculate_basis(parameter);
}

EvaluatedPoint get_evaluated_point(uint segment_id,
                                   uint curve_start,
                                   uint curve_end,
                                   float parameter)
{
  const int4 point_ids = get_points(segment_id, curve_start, curve_end);
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

}  // namespace catmull_rom

void main()
{
  uint curve_id = gl_GlobalInvocationID.x;
  if (curve_id >= uint(curves_count)) {
    return;
  }

  const uint curve_start = curves_offsets_buf[curve_id];
  const uint curve_end = curves_offsets_buf[curve_id + 1];
  const uint curve_resolution = curves_resolution_buf[curve_id];
  const CurveType curve_type = CurveType(curves_type_buf[curve_id]);

  const uint evaluated_start = curves_evaluated_offsets_buf[curve_id];
  const uint evaluated_end = curves_evaluated_offsets_buf[curve_id + 1];

  float3 last_position = float3(0.0f);
  float distance_along_curve = 0.0f;
  for (uint i = 0; i < evaluated_end - evaluated_start; i++) {
    const uint out_id = evaluated_start + i;
    const uint segment_id = i / curve_resolution;
    const float parameter = float(i % curve_resolution) / float(curve_resolution);

    EvaluatedPoint pt;
    switch (curve_type) {
      case CURVE_TYPE_CATMULL_ROM:
        pt = catmull_rom::get_evaluated_point(segment_id, curve_start, curve_end, parameter);
        break;
      case CURVE_TYPE_POLY:
      case CURVE_TYPE_BEZIER:
      case CURVE_TYPE_NURBS:
        break;
    }

    points_pos_rad_buf[out_id] = float4(pt.position, pt.radius);

    if (true /* TODO(fclem) Make it optional. */) {
      distance_along_curve += (i == 0) ? 0.0f : distance(last_position, pt.position);
      last_position = pt.position;
      points_time_buf[out_id] = distance_along_curve;
    }
  }

  if (true /* TODO(fclem) Make it optional. */) {
    curves_length_buf[curve_id] = distance_along_curve;
    for (uint i = evaluated_start; i < evaluated_end; i++) {
      points_time_buf[i] /= distance_along_curve;
    }
  }
}
