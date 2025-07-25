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

template<typename DataT> DataT interp_data(DataT v0, DataT v1, DataT v2, DataT v3, float4 w)
{
  return v0 * w.x + v1 * w.y + v2 * w.z + v3 * w.w;
}

template float interp_data<float>(float, float, float, float, float4);
template float2 interp_data<float2>(float2, float2, float2, float2, float4);
template float3 interp_data<float3>(float3, float3, float3, float3, float4);
template float4 interp_data<float4>(float4, float4, float4, float4, float4);

float4 get_weights_cardinal(float t)
{
  float t2 = t * t;
  float t3 = t2 * t;
#if defined(CARDINAL)
  float fc = 0.71f;
#else /* defined(CATMULL_ROM) */
  float fc = 0.5f;
#endif

  float4 weights;
  /* GLSL Optimized version of key_curve_position_weights() */
  float fct = t * fc;
  float fct2 = t2 * fc;
  float fct3 = t3 * fc;
  weights.x = (fct2 * 2.0f - fct3) - fct;
  weights.y = (t3 * 2.0f - fct3) + (-t2 * 3.0f + fct2) + 1.0f;
  weights.z = (-t3 * 2.0f + fct3) + (t2 * 3.0f - (2.0f * fct2)) + fct;
  weights.w = fct3 - fct2;
  return weights;
}

/* TODO(fclem): This one is buggy, find why. (it's not the optimization!!) */
float4 get_weights_bspline(float t)
{
  float t2 = t * t;
  float t3 = t2 * t;

  float4 weights;
  /* GLSL Optimized version of key_curve_position_weights() */
  weights.xz = float2(-0.16666666f, -0.5f) * t3 + (0.5f * t2 + 0.5f * float2(-t, t) + 0.16666666f);
  weights.y = (0.5f * t3 - t2 + 0.66666666f);
  weights.w = (0.16666666f * t3);
  return weights;
}

uint4 get_points(CurveType curve_type, uint pt_id, uint curve_start, uint curve_end)
{
  uint4 pt_ids = uint4(pt_id - 1, pt_id + 0, pt_id + 1, pt_id + 2);
  return clamp(curve_start + pt_ids, uint4(curve_start), uint4(curve_end - 1));
}

float4 get_weights(CurveType curve_type, float t)
{
  return get_weights_cardinal(t);
}

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

  for (uint i = 0; i < evaluated_end - evaluated_start; i++) {
    const uint out_id = evaluated_start + i;
    const uint point_id = i / curve_resolution;
    const float t = (i % curve_resolution) / float(curve_resolution - 1);

    const uint4 point_ids = get_points(curve_type, point_id, curve_start, curve_end);
    const float4 weights = get_weights(curve_type, t);

    const float3 lP_0 = gpu_attr_load_float3(points_pos_buf, int2(3, 0), point_ids.x);
    const float3 lP_1 = gpu_attr_load_float3(points_pos_buf, int2(3, 0), point_ids.y);
    const float3 lP_2 = gpu_attr_load_float3(points_pos_buf, int2(3, 0), point_ids.z);
    const float3 lP_3 = gpu_attr_load_float3(points_pos_buf, int2(3, 0), point_ids.w);

    const float rad_0 = points_rad_buf[point_ids.x];
    const float rad_1 = points_rad_buf[point_ids.y];
    const float rad_2 = points_rad_buf[point_ids.z];
    const float rad_3 = points_rad_buf[point_ids.w];

    const float3 lP = interp_data(lP_0, lP_1, lP_2, lP_3, weights);
    const float radius = interp_data(rad_0, rad_1, rad_2, rad_3, weights);

    points_pos_rad_buf[out_id] = float4(lP, radius);
    // points_time_buf[out_id] = 0.0f;
  }
}
