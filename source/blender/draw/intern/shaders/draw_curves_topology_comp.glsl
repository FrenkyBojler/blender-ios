/* SPDX-FileCopyrightText: 2021-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * GPU evaluated curve weights. Updated on topology change.
 * One thread processes one curve.
 *
 * This step outputs:
 * - Per final point: Curve index, Control point weights.
 * - Per curve: Final point range.
 */

#include "draw_curves_info.hh"

COMPUTE_SHADER_CREATE_INFO(draw_curves_topology_compute)

#include "draw_curves_lib.glsl"

template<typename DataT> DataT interp_data(DataT v0, DataT v1, DataT v2, DataT v3, float4 w)
{
  return v0 * w.x + v1 * w.y + v2 * w.z + v3 * w.w;
}

template float interp_data<float>(float, float, float, float, float4);
template float2 interp_data<float2>(float2, float2, float2, float2, float4);
template float3 interp_data<float3>(float3, float3, float3, float3, float4);
template float4 interp_data<float4>(float4, float4, float4, float4, float4);

/* Hair interpolation functions. */
float4 hair_get_weights_cardinal(float t)
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
float4 hair_get_weights_bspline(float t)
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

void main()
{
  uint curve_id = gl_GlobalInvocationID.x;
  if (curve_id >= curves_count) {
    return;
  }
  uint num_control_point = curves_offsets_buf[curve_id] - curves_offsets_buf[curve_id + 1];
  uint resolution = curves_resolution_buf[curve_id];
  /* TODO(fclem): Convert uint to bool4 with 1 byte per bool. */
  bool is_cyclic = false;  // curves_cyclic_buf[curve_id];

  uint num_segment = (num_control_point - 1) * resolution;

  uint point_id = atomicAdd(atomic_point_counter[0], num_segment);

  float curve_length = 0.0;
  float3 prev_coord;
  for (uint i = 0; i < num_segment; i++) {
    float t = float(i) / float(resolution);
    float t_seg = fract(t);
    uint first_control_point = uint(t);

    float4 weights;  // TODO = hair_get_weights_cardinal();

    if (compute_length) {
      float3 point_coord;  // TODO = curves::position_get(weights);
      if (i > 0) {
        curve_length += distance(prev_coord, point_coord);
      }
      points_time_buf[point_id] = curve_length;
      prev_coord = point_coord;
    }

    points_weights_buf[point_id] = weights;
    points_curve_id_buf[point_id] = curve_id;
    /* TODO(fclem): Merge weights for start and end of the curve. */
    point_id++;
  }

  if (compute_length) {
    curves_length_buf[curve_id] = curve_length;
  }
}
