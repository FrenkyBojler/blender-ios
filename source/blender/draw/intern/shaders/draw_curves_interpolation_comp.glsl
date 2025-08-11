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

/* We workaround the lack of function pointers by using different type to overload the attribute
 * implementation. */
struct InterpPosition {
  /* Position, Radius. */
  float4 data;
};

InterpPosition input_load(int point_index, InterpPosition interp)
{
  const auto &positions = buffer_get(draw_curves_interpolate_position, positions_buf);
  interp.data.xyz = gpu_attr_load_float3(positions, int2(3, 0), point_index);
  interp.data.w = buffer_get(draw_curves_interpolate_position, radii_buf)[point_index];
  return interp;
}

void output_weighted_add(int evaluated_point_index, float w, const InterpPosition interp)
{
  buffer_get(draw_curves_interpolate_position,
             evaluated_positions_radii_buf)[evaluated_point_index] += interp.data * w;
}
void output_mul(int evaluated_point_index, float w, const InterpPosition interp)
{
  buffer_get(draw_curves_interpolate_position,
             evaluated_positions_radii_buf)[evaluated_point_index] *= w;
}
void output_write(int evaluated_point_index, InterpPosition interp)
{
  buffer_get(draw_curves_interpolate_position,
             evaluated_positions_radii_buf)[evaluated_point_index] = interp.data;
}
void output_set_zero(int evaluated_point_index, InterpPosition interp)
{
  buffer_get(draw_curves_interpolate_position,
             evaluated_positions_radii_buf)[evaluated_point_index] = float4(0.0);
}

float4 input_load(int point_index, float4 interp)
{
  return load_data(
      buffer_get(draw_curves_interpolate_float4_attribute, attribute_float4_buf)[point_index]);
}

float3 input_load(int point_index, float3 interp)
{
  return load_data(
      buffer_get(draw_curves_interpolate_float3_attribute, attribute_float3_buf)[point_index]);
}

float2 input_load(int point_index, float2 interp)
{
  return load_data(
      buffer_get(draw_curves_interpolate_float2_attribute, attribute_float2_buf)[point_index]);
}

float input_load(int point_index, float interp)
{
  return load_data(
      buffer_get(draw_curves_interpolate_float_attribute, attribute_float_buf)[point_index]);
}

float4 output_load(int evaluated_point_index, float4 interp)
{
  return load_data(buffer_get(draw_curves_interpolate_float4_attribute,
                              evaluated_float4_buf)[evaluated_point_index]);
}

float3 output_load(int evaluated_point_index, float3 interp)
{
  return load_data(buffer_get(draw_curves_interpolate_float3_attribute,
                              evaluated_float3_buf)[evaluated_point_index]);
}

float2 output_load(int evaluated_point_index, float2 interp)
{
  return load_data(buffer_get(draw_curves_interpolate_float2_attribute,
                              evaluated_float2_buf)[evaluated_point_index]);
}

float output_load(int evaluated_point_index, float interp)
{
  return load_data(buffer_get(draw_curves_interpolate_float_attribute,
                              evaluated_float_buf)[evaluated_point_index]);
}

void output_write(int evaluated_point_index, const float4 interp)
{
  buffer_get(draw_curves_interpolate_float4_attribute,
             evaluated_float4_buf)[evaluated_point_index] = as_data(interp);
}

void output_write(int evaluated_point_index, const float3 interp)
{
  buffer_get(draw_curves_interpolate_float3_attribute,
             evaluated_float3_buf)[evaluated_point_index] = as_data(interp);
}

void output_write(int evaluated_point_index, const float2 interp)
{
  buffer_get(draw_curves_interpolate_float2_attribute,
             evaluated_float2_buf)[evaluated_point_index] = as_data(interp);
}

void output_write(int evaluated_point_index, const float interp)
{
  buffer_get(draw_curves_interpolate_float_attribute,
             evaluated_float_buf)[evaluated_point_index] = as_data(interp);
}

template<typename InterpType>
void output_weighted_add(int evaluated_point_index, float w, const InterpType src)
{
  InterpType dst = output_load(evaluated_point_index, src);
  dst += src * w;
  output_write(evaluated_point_index, dst);
}
template void output_weighted_add<float4>(int, float, float4);
template void output_weighted_add<float3>(int, float, float3);
template void output_weighted_add<float2>(int, float, float2);
template void output_weighted_add<float>(int, float, float);

template<typename InterpType>
void output_mul(int evaluated_point_index, float w, const InterpType type)
{
  InterpType dst = output_load(evaluated_point_index, type);
  dst *= w;
  output_write(evaluated_point_index, dst);
}
template void output_mul<float4>(int, float, float4);
template void output_mul<float3>(int, float, float3);
template void output_mul<float2>(int, float, float2);
template void output_mul<float>(int, float, float);

template<typename InterpType>
void output_set_zero(int evaluated_point_index, const InterpType interp)
{
  output_write(evaluated_point_index, InterpType(0.0f));
}
template void output_set_zero<float4>(int, float4);
template void output_set_zero<float3>(int, float3);
template void output_set_zero<float2>(int, float2);
template void output_set_zero<float>(int, float);

class IndexRange {
 private:
  int start_;
  int size_;

 public:
  METAL_CONSTRUCTOR_2(IndexRange, int, start_, int, size_)

  static IndexRange from_begin_end(int begin, int end)
  {
    return IndexRange(begin, end - begin);
  }

  /**
   * Get the first element in the range.
   */
  int first() const
  {
    return this->start_;
  }

  /**
   * Get the first element in the range. The returned value is undefined when the range is empty.
   */
  int start() const
  {
    return this->start_;
  }

  /**
   * Get the nth last element in the range.
   */
  int last(int n = 0) const
  {
    return this->start_ + this->size_ - 1 - n;
  }

  /**
   * Get the amount of numbers in the range.
   */
  int size() const
  {
    return this->size_;
  }

  /**
   * Returns a new range, that contains a sub-interval of the current one.
   */
  IndexRange slice(int start, int size) const
  {
    int new_start = this->start_ + start;
    return IndexRange(new_start, size);
  }
  IndexRange slice(IndexRange range) const
  {
    return this->slice(range.start(), range.size());
  }
};

/**
 * See `OffsetIndices` C++ definition for formal definition.
 *
 * OffsetIndices cannot be implemented on GPU because of the lack of operator overloading and
 * buffer reference in GLSL. So we simply interpret a given integer buffer as a `OffsetIndices`
 * buffer and load a specific item as a range.
 */
namespace offset_indices {

#ifdef GLSL_CPP_STUBS
/* Equivalent of `IndexRange OffsetIndices<int>operator[]`.
 * Implementation for C++ compilation. */
static IndexRange load_range_from_buffer(const int (&buf)[], int i)
{
  return IndexRange::from_begin_end(buf[i], buf[i + 1]);
}
#endif

}  // namespace offset_indices

/* Shader implementation because of missing buffer reference as argument in GLSL. */
#define offset_indices_load_range_from_buffer(buf_, i_) \
  IndexRange::from_begin_end(buf_[i_], buf_[i_ + 1]);

/* Copy of DNA enum in `DNA_curves_types.h`. */
enum CurveType : uint32_t {
  CURVE_TYPE_CATMULL_ROM = 0u,
  CURVE_TYPE_POLY = 1u,
  CURVE_TYPE_BEZIER = 2u,
  CURVE_TYPE_NURBS = 3u,
};

InterpPosition mix4(
    InterpPosition v0, InterpPosition v1, InterpPosition v2, InterpPosition v3, float4 w)
{
  v0.data = v0.data * w.x + v1.data * w.y + v2.data * w.z + v3.data * w.w;
  return v0;
}

template<typename DataT> DataT mix4(DataT v0, DataT v1, DataT v2, DataT v3, float4 w)
{
  v0 = v0 * w.x + v1 * w.y + v2 * w.z + v3 * w.w;
  return v0;
}
template float4 mix4<float4>(float4, float4, float4, float4, float4);
template float3 mix4<float3>(float3, float3, float3, float3, float4);
template float2 mix4<float2>(float2, float2, float2, float2, float4);
template float mix4<float>(float, float, float, float, float4);

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
      int(points.start()) + point_ids, int4(points.start()), int4(points.start() + points.last()));
}

template<typename InterpType>
void evaluate_curve(const InterpType interp_type,
                    const IndexRange points,
                    const IndexRange evaluated_points,
                    const int curve_index)
{
  const uint curve_resolution = curves_resolution_buf[curve_index];

  for (uint i = 0; i < evaluated_points.size(); i++) {
    const int evaluated_point_id = evaluated_points.start() + int(i);
    const uint point_id = i / curve_resolution;
    const float parameter = float(i % curve_resolution) / float(curve_resolution);
    const float4 weights = calculate_basis(parameter);
    const int4 point_ids = get_points(point_id, points);

    InterpType p0 = input_load(point_ids.x, interp_type);
    InterpType p1 = input_load(point_ids.y, interp_type);
    InterpType p2 = input_load(point_ids.z, interp_type);
    InterpType p3 = input_load(point_ids.w, interp_type);
    InterpType result = mix4(p0, p1, p2, p3, weights);

    output_write(evaluated_point_id, result);
  }
}

template void evaluate_curve<InterpPosition>(InterpPosition, IndexRange, IndexRange, int);
template void evaluate_curve<float>(float, IndexRange, IndexRange, int);
template void evaluate_curve<float2>(float2, IndexRange, IndexRange, int);
template void evaluate_curve<float3>(float3, IndexRange, IndexRange, int);
template void evaluate_curve<float4>(float4, IndexRange, IndexRange, int);

}  // namespace catmull_rom

namespace bezier {

void evaluate_segment(const InterpPosition interp_type, const int2 points, const IndexRange result)
{
  const auto &handles_right = buffer_get(draw_curves_interpolate_position,
                                         handles_positions_right_buf);
  const auto &handles_left = buffer_get(draw_curves_interpolate_position,
                                        handles_positions_left_buf);

  InterpPosition p0 = input_load(points.x, interp_type);
  InterpPosition p1 = input_load(points.y, interp_type);

  const float3 point_0 = p0.data.xyz;
  const float3 point_1 = gpu_attr_load_float3(handles_right, int2(3, 0), points.x);
  const float3 point_2 = gpu_attr_load_float3(handles_left, int2(3, 0), points.y);
  const float3 point_3 = p1.data.xyz;

  const float rad_0 = p0.data.w;
  const float rad_1 = p1.data.w;

  assert(result.size > 0);
  const float inv_len = 1.0f / float(result.size());
  const float inv_len_squared = inv_len * inv_len;
  const float inv_len_cubed = inv_len_squared * inv_len;

  const float3 rt1 = 3.0f * (point_1 - point_0) * inv_len;
  const float3 rt2 = 3.0f * (point_0 - 2.0f * point_1 + point_2) * inv_len_squared;
  const float3 rt3 = (point_3 - point_0 + 3.0f * (point_1 - point_2)) * inv_len_cubed;

  float3 q0 = point_0;
  float3 q1 = rt1 + rt2 + rt3;
  float3 q2 = 2.0f * rt2 + 6.0f * rt3;
  float3 q3 = 6.0f * rt3;
  for (int i = 0; i < result.size(); i++) {
    float rad = mix(rad_0, rad_1, float(i) * inv_len);
    InterpPosition interp;
    interp.data = float4(q0, rad);
    output_write(result.start() + i, interp);
    q0 += q1;
    q1 += q2;
    q2 += q3;
  }
}

template<typename InterpType>
void evaluate_segment(const InterpType interp_type, const int2 points, const IndexRange result)
{
  InterpType p0 = input_load(points.x, interp_type);
  InterpType p1 = input_load(points.y, interp_type);

  const float step = 1.0f / float(result.size());
  for (int i = 0; i < result.size(); i++) {
    output_write(result.start() + i, mix(p0, p1, float(i) * step));
  }
}

template void evaluate_segment<float>(float, int2, IndexRange);
template void evaluate_segment<float2>(float2, int2, IndexRange);
template void evaluate_segment<float3>(float3, int2, IndexRange);
template void evaluate_segment<float4>(float4, int2, IndexRange);

IndexRange per_curve_point_offsets_range(const IndexRange points, const int curve_index)
{
  return IndexRange(curve_index + points.start(), points.size() + 1);
}

int2 get_points(uint point_id, IndexRange points)
{
  int2 point_ids = int(point_id) + int2(+0, +1);
  return clamp(int(points.start()) + point_ids,
               int2(points.start()),
               int2(points.start() + points.size() - 1));
}

template<typename InterpType>
void evaluate_curve(const InterpType interp_type,
                    const IndexRange points,
                    const IndexRange evaluated_points,
                    const int curve_index)
{
  /* Range used for indexing bezier offsets. */
  const IndexRange offsets = per_curve_point_offsets_range(points, curve_index);

  for (int i = 0; i < points.size(); i++) {
    /* Bezier curves can have different number of evaluated segment per curve segment. */
    const IndexRange segment_range = offset_indices::load_range_from_buffer(bezier_offsets_buf,
                                                                            offsets.start() + i);
    const IndexRange evaluated_segment_range = evaluated_points.slice(segment_range);
    const int2 point_ids = get_points(i, points);

    evaluate_segment(interp_type, point_ids, evaluated_segment_range);
  }
}

template void evaluate_curve<InterpPosition>(InterpPosition, IndexRange, IndexRange, int);
template void evaluate_curve<float>(float, IndexRange, IndexRange, int);
template void evaluate_curve<float2>(float2, IndexRange, IndexRange, int);
template void evaluate_curve<float3>(float3, IndexRange, IndexRange, int);
template void evaluate_curve<float4>(float4, IndexRange, IndexRange, int);

}  // namespace bezier

template<typename InterpType>
void copy_curve_data(const InterpType interp_type,
                     const IndexRange points,
                     const IndexRange evaluated_points)
{
  assert(points.size == evaluated_points.size);
  for (int i = 0; i < points.size(); i++) {
    output_write(evaluated_points.start() + i, input_load(points.start() + i, interp_type));
  }
}

template void copy_curve_data<InterpPosition>(InterpPosition, IndexRange, IndexRange);
template void copy_curve_data<float>(float, IndexRange, IndexRange);
template void copy_curve_data<float2>(float2, IndexRange, IndexRange);
template void copy_curve_data<float3>(float3, IndexRange, IndexRange);
template void copy_curve_data<float4>(float4, IndexRange, IndexRange);

namespace nurbs {

template<typename InterpType>
void evaluate_curve(const InterpType interp_type,
                    const IndexRange points,
                    const IndexRange evaluated_points,
                    const uint curve_index)
{
  /* Buffer aliasing to same bind point. We cannot dispatch with different type of curve. */
  const auto &curves_order_buf = curves_resolution_buf;
  const auto &basis_cache_offset_buf = bezier_offsets_buf;

  const int order = int(gpu_attr_load_uchar(curves_order_buf, curve_index));

  const int basis_cache_start = basis_cache_offset_buf[curve_index];
  const bool invalid = basis_cache_start < 0;

  if (invalid) {
    copy_curve_data(interp_type, points, evaluated_points);
    return;
  }

  const int start_indices_range_start = basis_cache_start;
  const int weights_range_start = basis_cache_start + evaluated_points.size();

  /* Buffer aliasing to same bind point. We cannot dispatch with different type of curve. */
  const auto &basis_cache_buf = handles_positions_left_buf;
  const auto &control_weights_buf = handles_positions_right_buf;

  for (int i = 0; i < evaluated_points.size(); i++) {
    int evaluated_point_index = evaluated_points.start() + i;
    /* Equivalent to `attribute_math::DefaultMixer<T> mixer{dst}`. */
    output_set_zero(evaluated_point_index, interp_type);
    float total_weight = 0.0f;

    const IndexRange point_weights = IndexRange(weights_range_start + i * order, order);
    const int start_index = floatBitsToInt(basis_cache_buf[start_indices_range_start + i]);

    for (int j = 0; j < point_weights.size(); j++) {
      const int point_index = points.start() + (start_index + j) % points.size();
      const float point_weight = basis_cache_buf[point_weights.start() + j];
      const float control_weight = use_point_weight ? control_weights_buf[point_index] : 1.0f;
      const float weight = point_weight * control_weight;
      /* Equivalent to `mixer.mix_in()`. */
      output_weighted_add(evaluated_point_index, weight, input_load(point_index, interp_type));
      total_weight += weight;
    }
    /* Equivalent to `mixer.finalize()` */
    output_mul(evaluated_point_index, safe_rcp(total_weight), interp_type);
  }
}

template void evaluate_curve<InterpPosition>(InterpPosition, IndexRange, IndexRange, uint);
template void evaluate_curve<float>(float, IndexRange, IndexRange, uint);
template void evaluate_curve<float2>(float2, IndexRange, IndexRange, uint);
template void evaluate_curve<float3>(float3, IndexRange, IndexRange, uint);
template void evaluate_curve<float4>(float4, IndexRange, IndexRange, uint);

}  // namespace nurbs

/* Run on the evaluated position and compute the intercept time with the curve and the total curve
 * length. */
void evaluate_length_and_time(const IndexRange evaluated_points, const int curve_index)
{
  auto &evaluated_positions_radii = buffer_get(draw_curves_interpolate_position,
                                               evaluated_positions_radii_buf);
  auto &evaluated_time = buffer_get(draw_curves_interpolate_position, evaluated_time_buf);
  auto &curves_length = buffer_get(draw_curves_interpolate_position, curves_length_buf);

  float distance_along_curve = 0.0f;
  evaluated_time[0] = 0.0f;
  for (int i = 1; i < evaluated_points.size(); i++) {
    int p = evaluated_points.start() + i;
    distance_along_curve += distance(evaluated_positions_radii[p].xyz,
                                     evaluated_positions_radii[p - 1].xyz);
    evaluated_time[p] = distance_along_curve;
  }
  for (int i = 1; i < evaluated_points.size(); i++) {
    int p = evaluated_points.start() + i;
    evaluated_time[p] /= distance_along_curve;
  }
  curves_length[curve_index] = distance_along_curve;
}

template<typename InterpType> void evaluate_curve(const InterpType interp_type)
{
  if (gl_GlobalInvocationID.x >= uint(curves_count)) {
    return;
  }
  int curve_index = int(gl_GlobalInvocationID.x) + curves_start;

  const CurveType curve_type = CurveType(gpu_attr_load_uchar(curves_type_buf, curve_index));
  if (curve_type != CurveType(evaluated_type)) {
    return;
  }
  IndexRange points = offset_indices::load_range_from_buffer(points_by_curve_buf, curve_index);
  IndexRange evaluated_points = offset_indices::load_range_from_buffer(
      evaluated_points_by_curve_buf, curve_index);

  if (CurveType(evaluated_type) == CURVE_TYPE_CATMULL_ROM) {
    catmull_rom::evaluate_curve(interp_type, points, evaluated_points, curve_index);
  }
  else if (CurveType(evaluated_type) == CURVE_TYPE_BEZIER) {
    bezier::evaluate_curve(interp_type, points, evaluated_points, curve_index);
  }
  else if (CurveType(evaluated_type) == CURVE_TYPE_NURBS) {
    nurbs::evaluate_curve(interp_type, points, evaluated_points, uint(curve_index));
  }
  else if (CurveType(evaluated_type) == CURVE_TYPE_POLY) {
    /* Simple copy. */
    copy_curve_data(interp_type, points, evaluated_points);
  }

  if (compute_length_and_time) {
    evaluate_length_and_time(evaluated_points, curve_index);
  }
}

template void evaluate_curve<InterpPosition>(InterpPosition);
template void evaluate_curve<float>(float);
template void evaluate_curve<float2>(float2);
template void evaluate_curve<float3>(float3);
template void evaluate_curve<float4>(float4);

void evaluate_position_radius()
{
  InterpPosition interp_type;
  interp_type.data = float4(0.0f); /* Avoid warnings. */
  evaluate_curve(interp_type);
}

void evaluate_attribute_float()
{
  evaluate_curve(float(0.0f));
}

void evaluate_attribute_float2()
{
  evaluate_curve(float2(0.0f));
}

void evaluate_attribute_float3()
{
  evaluate_curve(float3(0.0f));
}

void evaluate_attribute_float4()
{
  evaluate_curve(float4(0.0f));
}
