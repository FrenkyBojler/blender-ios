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

COMPUTE_SHADER_CREATE_INFO(draw_curves_interpolate_position)

#include "gpu_shader_attribute_load_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"

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

/* Equivalent of `IndexRange OffsetIndices<int>operator[]`. */
#define OffsetIndices_read(buf_, i_) from_begin_end(buf_[i_], buf_[i_ + 1]);

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

int4 get_points(uint point_id, IndexRange points)
{
  int4 point_ids = int(point_id) + int4(-1, +0, +1, +2);
  return clamp(
      int(points.start) + point_ids, int4(points.start), int4(points.start + points.size - 1));
}

float3 get_evaluated_position(const int4 point_ids, const float4 weights)
{
  const float3 pos_0 = gpu_attr_load_float3(positions_buf, int2(3, 0), point_ids.x);
  const float3 pos_1 = gpu_attr_load_float3(positions_buf, int2(3, 0), point_ids.y);
  const float3 pos_2 = gpu_attr_load_float3(positions_buf, int2(3, 0), point_ids.z);
  const float3 pos_3 = gpu_attr_load_float3(positions_buf, int2(3, 0), point_ids.w);
  return mix4(pos_0, pos_1, pos_2, pos_3, weights);
}

float get_evaluated_radius(const int4 point_ids, const float4 weights)
{
  const float rad_0 = radii_buf[point_ids.x];
  const float rad_1 = radii_buf[point_ids.y];
  const float rad_2 = radii_buf[point_ids.z];
  const float rad_3 = radii_buf[point_ids.w];
  return mix4(rad_0, rad_1, rad_2, rad_3, weights);
}

void evaluate_curve(IndexRange points, IndexRange evaluated_points, int curve_index)
{
  const uint curve_resolution = curves_resolution_buf[curve_index];

  for (uint i = 0; i < evaluated_points.size; i++) {
    const uint evaluated_point_id = evaluated_points.start + i;
    const uint point_id = i / curve_resolution;
    const float parameter = float(i % curve_resolution) / float(curve_resolution);
    const float4 weights = calculate_basis(parameter);
    const int4 point_ids = get_points(point_id, points);

    float3 position = get_evaluated_position(point_ids, weights);
    float radius = get_evaluated_radius(point_ids, weights);
    evaluated_positions_radii_buf[evaluated_point_id] = float4(position, radius);
  }
}

}  // namespace catmull_rom

namespace bezier {

IndexRange per_curve_point_offsets_range(const IndexRange points, const int curve_index)
{
  return IndexRange(curve_index + points.start, points.size + 1);
}

int2 get_points(uint point_id, IndexRange points)
{
  int2 point_ids = int(point_id) + int2(+0, +1);
  return clamp(
      int(points.start) + point_ids, int2(points.start), int2(points.start + points.size - 1));
}

void evaluate_segment_positions(const int2 points, const IndexRange result)
{
  const float3 point_0 = gpu_attr_load_float3(positions_buf, int2(3, 0), points.x);
  const float3 point_1 = gpu_attr_load_float3(handles_positions_right_buf, int2(3, 0), points.x);
  const float3 point_2 = gpu_attr_load_float3(handles_positions_left_buf, int2(3, 0), points.y);
  const float3 point_3 = gpu_attr_load_float3(positions_buf, int2(3, 0), points.y);

  const float rad_0 = radii_buf[points.x];
  const float rad_1 = radii_buf[points.y];

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
    /* Radius is done separately. */
    evaluated_positions_radii_buf[result.start + i].xyz = q0;
    evaluated_positions_radii_buf[result.start + i].w = mix(rad_0, rad_1, float(i) * inv_len);
    q0 += q1;
    q1 += q2;
    q2 += q3;
  }
}

void evaluate_curve(const IndexRange points,
                    const IndexRange evaluated_points,
                    const int curve_index)
{
  /* Range used for indexing bezier offsets. */
  const IndexRange offsets = per_curve_point_offsets_range(points, curve_index);

  for (int i = 0; i < points.size; i++) {
    /* Bezier curves can have different number of evaluated segment per curve segment. */
    const IndexRange segment_range = OffsetIndices_read(bezier_offsets_buf, offsets.start + i);
    const IndexRange evaluated_segment_range = slice(evaluated_points, segment_range);
    const int2 point_ids = get_points(i, points);

    evaluate_segment_positions(point_ids, evaluated_segment_range);
  }
}

}  // namespace bezier

void copy_curve_data(const IndexRange points, const IndexRange evaluated_points)
{
  assert(points.size == evaluated_points.size);
  for (int i = 0; i < points.size; i++) {
    float3 position = gpu_attr_load_float3(positions_buf, int2(3, 0), points.start + i);
    float radius = radii_buf[points.start + i];
    evaluated_positions_radii_buf[evaluated_points.start + i] = float4(position, radius);
  }
}

namespace nurbs {

void interpolate_to_evaluated_rational(const IndexRange points,
                                       const IndexRange evaluated_points,
                                       uint curve_index)
{
  /* Buffer aliasing to same bind point. We cannot dispatch with different type of curve. */
  const auto &curves_order_buf = curves_resolution_buf;
  const auto &basis_cache_buf = handles_positions_left_buf;
  const auto &control_weights_buf = handles_positions_right_buf;
  const auto &basis_cache_offset_buf = bezier_offsets_buf;

  const int order = int(gpu_attr_load_uchar(curves_order_buf, curve_index));

  const int basis_cache_start = basis_cache_offset_buf[curve_index];
  const bool invalid = floatBitsToInt(basis_cache_buf[basis_cache_start]) != 0;
  const IndexRange start_indices_range = IndexRange(basis_cache_start + 1, evaluated_points.size);
  const IndexRange weights_range = IndexRange(basis_cache_start + 1 + evaluated_points.size,
                                              evaluated_points.size * order);

  if (invalid) {
    copy_curve_data(points, evaluated_points);
    return;
  }

  for (int i = 0; i < evaluated_points.size; i++) {
    /* Equivalent to `attribute_math::DefaultMixer<T> mixer{dst}`. */
    evaluated_positions_radii_buf[evaluated_points.start + i] = float4(0.0f);
    float total_weight = 0.0f;

    const IndexRange point_weights = slice(weights_range, IndexRange(i * order, order));
    const int start_index = floatBitsToInt(basis_cache_buf[start_indices_range.start + i]);

    for (int j = 0; j < point_weights.size; j++) {
      const int point_index = points.start + (start_index + j) % points.size;
      const float point_weight = basis_cache_buf[point_weights.start + j];
      const float weight = point_weight * control_weights_buf[point_index];

      const float3 pos = gpu_attr_load_float3(positions_buf, int2(3, 0), point_index);
      const float rad = radii_buf[point_index];

      /* Equivalent to `mixer.mix_in()`. */
      evaluated_positions_radii_buf[evaluated_points.start + i] += float4(pos, rad) * weight;
      total_weight += weight;
    }
    /* Equivalent to `mixer.finalize()` */
    evaluated_positions_radii_buf[evaluated_points.start + i] *= safe_rcp(total_weight);
  }
}

}  // namespace nurbs

void main()
{
  int curve_index = int(gl_GlobalInvocationID.x);
  if (curve_index >= curves_count) {
    return;
  }

  const CurveType curve_type = CurveType(curves_type_buf[curve_index]);
  if (curve_type != CurveType(evaluated_type)) {
    return;
  }

  IndexRange points = OffsetIndices_read(points_by_curve_buf, curve_index);
  IndexRange evaluated_points = OffsetIndices_read(evaluated_points_by_curve_buf, curve_index);

  if (CurveType(evaluated_type) == CURVE_TYPE_CATMULL_ROM) {
    catmull_rom::evaluate_curve(points, evaluated_points, curve_index);
  }
  else if (CurveType(evaluated_type) == CURVE_TYPE_BEZIER) {
    bezier::evaluate_curve(points, evaluated_points, curve_index);
  }
  else if (CurveType(evaluated_type) == CURVE_TYPE_NURBS) {
    nurbs::interpolate_to_evaluated_rational(points, evaluated_points, uint(curve_index));
  }
  else if (CurveType(evaluated_type) == CURVE_TYPE_POLY) {
    /* Simple copy. */
    copy_curve_data(points, evaluated_points);
  }

  if (compute_length_and_time) {
    float distance_along_curve = 0.0f;
    evaluated_time_buf[0] = 0.0f;
    for (int i = 1; i < evaluated_points.size; i++) {
      int p = evaluated_points.start + i;
      distance_along_curve += distance(evaluated_positions_radii_buf[p].xyz,
                                       evaluated_positions_radii_buf[p - 1].xyz);
      evaluated_time_buf[p] = distance_along_curve;
    }
    curves_length_buf[curve_index] = distance_along_curve;
    for (int i = 1; i < evaluated_points.size; i++) {
      int p = evaluated_points.start + i;
      evaluated_time_buf[p] /= distance_along_curve;
    }
  }
}
