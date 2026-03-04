/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <algorithm>

#include "BLI_array_utils.hh"
#include "BLI_math_euler.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion.hh"

#include "BKE_attribute_math.hh"

namespace blender::bke::attribute_math {

template<>
math::Quaternion mix2(const float factor, const math::Quaternion &a, const math::Quaternion &b)
{
  return math::interpolate(a, b, factor);
}

template<>
math::Quaternion mix3(const float3 &weights,
                      const math::Quaternion &v0,
                      const math::Quaternion &v1,
                      const math::Quaternion &v2)
{
  const float3 expmap_mixed = mix3(weights, v0.expmap(), v1.expmap(), v2.expmap());
  return math::Quaternion::expmap(expmap_mixed);
}

template<>
math::Quaternion mix4(const float4 &weights,
                      const math::Quaternion &v0,
                      const math::Quaternion &v1,
                      const math::Quaternion &v2,
                      const math::Quaternion &v3)
{
  const float3 expmap_mixed = mix4(weights, v0.expmap(), v1.expmap(), v2.expmap(), v3.expmap());
  return math::Quaternion::expmap(expmap_mixed);
}

template<> float4x4 mix2(const float factor, const float4x4 &a, const float4x4 &b)
{
  return math::interpolate(a, b, factor);
}

template<>
float4x4 mix3(const float3 &weights, const float4x4 &v0, const float4x4 &v1, const float4x4 &v2)
{
  const float3 location = mix3(weights, v0.location(), v1.location(), v2.location());
  const math::Quaternion rotation = mix3(
      weights,
      math::normalized_to_quaternion_safe(math::normalize(float3x3(v0))),
      math::normalized_to_quaternion_safe(math::normalize(float3x3(v1))),
      math::normalized_to_quaternion_safe(math::normalize(float3x3(v2))));
  const float3 scale = mix3(weights, math::to_scale(v0), math::to_scale(v1), math::to_scale(v2));
  return math::from_loc_rot_scale<float4x4>(location, rotation, scale);
}

template<>
float4x4 mix4(const float4 &weights,
              const float4x4 &v0,
              const float4x4 &v1,
              const float4x4 &v2,
              const float4x4 &v3)
{
  const float3 location = mix4(
      weights, v0.location(), v1.location(), v2.location(), v3.location());
  const math::Quaternion rotation = mix4(weights,
                                         math::to_quaternion(v0),
                                         math::to_quaternion(v1),
                                         math::to_quaternion(v2),
                                         math::to_quaternion(v3));
  const float3 scale = mix4(
      weights, math::to_scale(v0), math::to_scale(v1), math::to_scale(v2), math::to_scale(v3));
  return math::from_loc_rot_scale<float4x4>(location, rotation, scale);
}

ColorGeometry4fMixer::ColorGeometry4fMixer(MutableSpan<ColorGeometry4f> buffer,
                                           ColorGeometry4f default_color)
    : ColorGeometry4fMixer(buffer, buffer.index_range(), default_color)
{
}

ColorGeometry4fMixer::ColorGeometry4fMixer(MutableSpan<ColorGeometry4f> buffer,
                                           const IndexMask &mask,
                                           const ColorGeometry4f default_color)
    : buffer_(buffer), default_color_(default_color), total_weights_(buffer.size(), 0.0f)
{
  const ColorGeometry4f zero{0.0f, 0.0f, 0.0f, 0.0f};
  index_mask::masked_fill(buffer_, zero, mask);
}

void ColorGeometry4fMixer::set(const int64_t index,
                               const ColorGeometry4f &color,
                               const float weight)
{
  buffer_[index].r = color.r * weight;
  buffer_[index].g = color.g * weight;
  buffer_[index].b = color.b * weight;
  buffer_[index].a = color.a * weight;
  total_weights_[index] = weight;
}

void ColorGeometry4fMixer::mix_in(const int64_t index,
                                  const ColorGeometry4f &color,
                                  const float weight)
{
  ColorGeometry4f &output_color = buffer_[index];
  output_color.r += color.r * weight;
  output_color.g += color.g * weight;
  output_color.b += color.b * weight;
  output_color.a += color.a * weight;
  total_weights_[index] += weight;
}

void ColorGeometry4fMixer::finalize()
{
  this->finalize(buffer_.index_range());
}

void ColorGeometry4fMixer::finalize(const IndexMask &mask)
{
  mask.foreach_index([&](const int64_t i) {
    const float weight = total_weights_[i];
    ColorGeometry4f &output_color = buffer_[i];
    if (weight > 0.0f) {
      const float weight_inv = 1.0f / weight;
      output_color.r *= weight_inv;
      output_color.g *= weight_inv;
      output_color.b *= weight_inv;
      output_color.a *= weight_inv;
    }
    else {
      output_color = default_color_;
    }
  });
}

ColorGeometry4bMixer::ColorGeometry4bMixer(MutableSpan<ColorGeometry4b> buffer,
                                           const ColorGeometry4b default_color)
    : ColorGeometry4bMixer(buffer, buffer.index_range(), default_color)
{
}

ColorGeometry4bMixer::ColorGeometry4bMixer(MutableSpan<ColorGeometry4b> buffer,
                                           const IndexMask &mask,
                                           const ColorGeometry4b default_color)
    : buffer_(buffer),
      default_color_(default_color),
      total_weights_(buffer.size(), 0.0f),
      accumulation_buffer_(buffer.size(), float4(0, 0, 0, 0))
{
  const ColorGeometry4b zero{0, 0, 0, 0};
  index_mask::masked_fill(buffer_, zero, mask);
}

void ColorGeometry4bMixer::ColorGeometry4bMixer::set(int64_t index,
                                                     const ColorGeometry4b &color,
                                                     const float weight)
{
  accumulation_buffer_[index][0] = color.r * weight;
  accumulation_buffer_[index][1] = color.g * weight;
  accumulation_buffer_[index][2] = color.b * weight;
  accumulation_buffer_[index][3] = color.a * weight;
  total_weights_[index] = weight;
}

void ColorGeometry4bMixer::mix_in(int64_t index, const ColorGeometry4b &color, float weight)
{
  float4 &accum_value = accumulation_buffer_[index];
  accum_value[0] += color.r * weight;
  accum_value[1] += color.g * weight;
  accum_value[2] += color.b * weight;
  accum_value[3] += color.a * weight;
  total_weights_[index] += weight;
}

void ColorGeometry4bMixer::finalize()
{
  this->finalize(buffer_.index_range());
}

void ColorGeometry4bMixer::finalize(const IndexMask &mask)
{
  mask.foreach_index([&](const int64_t i) {
    const float weight = total_weights_[i];
    const float4 &accum_value = accumulation_buffer_[i];
    ColorGeometry4b &output_color = buffer_[i];
    if (weight > 0.0f) {
      const float weight_inv = 1.0f / weight;
      output_color.r = accum_value[0] * weight_inv;
      output_color.g = accum_value[1] * weight_inv;
      output_color.b = accum_value[2] * weight_inv;
      output_color.a = accum_value[3] * weight_inv;
    }
    else {
      output_color = default_color_;
    }
  });
}

float4x4Mixer::float4x4Mixer(MutableSpan<float4x4> buffer)
    : float4x4Mixer(buffer, buffer.index_range())
{
}

float4x4Mixer::float4x4Mixer(MutableSpan<float4x4> buffer, const IndexMask & /*mask*/)
    : buffer_(buffer),
      total_weights_(buffer.size(), 0.0f),
      location_buffer_(buffer.size(), float3(0)),
      expmap_buffer_(buffer.size(), float3(0)),
      scale_buffer_(buffer.size(), float3(0))
{
}

void float4x4Mixer::float4x4Mixer::set(int64_t index, const float4x4 &value, const float weight)
{
  location_buffer_[index] = value.location() * weight;
  expmap_buffer_[index] = math::to_quaternion(value).expmap() * weight;
  scale_buffer_[index] = math::to_scale(value) * weight;
  total_weights_[index] = weight;
}

void float4x4Mixer::mix_in(int64_t index, const float4x4 &value, float weight)
{
  float3 location;
  math::Quaternion rotation;
  float3 scale;
  math::to_loc_rot_scale_safe<true>(value, location, rotation, scale);

  location_buffer_[index] += location * weight;
  expmap_buffer_[index] += rotation.expmap() * weight;
  scale_buffer_[index] += scale * weight;
  total_weights_[index] += weight;
}

void float4x4Mixer::finalize()
{
  this->finalize(buffer_.index_range());
}

void float4x4Mixer::finalize(const IndexMask &mask)
{
  mask.foreach_index([&](const int64_t i) {
    const float weight = total_weights_[i];
    if (weight > 0.0f) {
      const float weight_inv = math::rcp(weight);
      buffer_[i] = math::from_loc_rot_scale<float4x4>(
          location_buffer_[i] * weight_inv,
          math::Quaternion::expmap(expmap_buffer_[i] * weight_inv),
          scale_buffer_[i] * weight_inv);
    }
    else {
      buffer_[i] = float4x4::identity();
    }
  });
}

template<typename T>
void mix_groups(const Span<T> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                MutableSpan<T> dst)
{
  for (const int dst_i : dst.index_range()) {
    const float weight = math::rcp(float(groups[dst_i].size()));
    T accum(0);
    for (const int src_i : all_indices.slice(groups[dst_i])) {
      accum += src[src_i] * weight;
    }
    dst[dst_i] = accum;
  }
}

template<>
void mix_groups(const Span<bool> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                MutableSpan<bool> dst)
{
  for (const int dst_i : groups.index_range()) {
    dst[dst_i] = std::ranges::any_of(all_indices.slice(groups[dst_i]),
                                     [&](const int i) { return src[i]; });
  }
}

template<typename T, typename ToAccumFn, typename ToFinalFn>
void mix_groups(const Span<T> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const ToAccumFn &to_accum_fn,
                const ToFinalFn &to_final_fn,
                MutableSpan<T> dst)
{
  using AccumT = std::invoke_result_t<ToAccumFn, T>;
  static_assert(std::is_same_v<std::invoke_result_t<ToFinalFn, AccumT>, T>);
  for (const int dst_i : dst.index_range()) {
    const float weight = math::rcp(float(groups[dst_i].size()));
    AccumT accum(0);
    for (const int src_i : all_indices.slice(groups[dst_i])) {
      accum += to_accum_fn(src[src_i]) * weight;
    }
    dst[dst_i] = to_final_fn(accum);
  }
}

static double int_to_double(const int &value)
{
  return double(value);
}
static int double_to_int(const double &value)
{
  return int(std::round(value));
}

template<>
void mix_groups(const Span<int> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                MutableSpan<int> dst)
{
  mix_groups(src, groups, all_indices, int_to_double, double_to_int, dst);
}

static double2 int2_to_double2(const int2 &value)
{
  return double2(value);
}
static int2 double2_to_int2(const double2 &value)
{
  return int2(math::round(value));
}

template<>
void mix_groups(const Span<int2> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                MutableSpan<int2> dst)
{
  mix_groups(src, groups, all_indices, int2_to_double2, double2_to_int2, dst);
}

static float int8_t_to_float(const int8_t &value)
{
  return float(value);
}
static int8_t float_to_int8_t(const float &value)
{
  return int8_t(std::round(value));
}

template<>
void mix_groups(const Span<int8_t> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                MutableSpan<int8_t> dst)
{
  mix_groups(src, groups, all_indices, int8_t_to_float, float_to_int8_t, dst);
}

static float2 short2_to_float2(const short2 &value)
{
  return float2(value);
}
static short2 float2_to_short2(const float2 &value)
{
  return short2(math::round(value));
}

template<>
void mix_groups(const Span<short2> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                MutableSpan<short2> dst)
{
  mix_groups(src, groups, all_indices, short2_to_float2, float2_to_short2, dst);
}

static float3 quat_to_expmap(const math::Quaternion &value)
{
  return value.expmap();
}
static math::Quaternion expmap_to_quat(const float3 &value)
{
  return math::Quaternion::expmap(value);
}

template<>
void mix_groups(const Span<math::Quaternion> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                MutableSpan<math::Quaternion> dst)
{
  mix_groups(src, groups, all_indices, quat_to_expmap, expmap_to_quat, dst);
}

static float4 byte_color_to_float4(const ColorGeometry4b &value)
{
  return float4(value.r, value.g, value.b, value.a);
}
static ColorGeometry4b float4_to_byte_color(const float4 &value)
{
  return ColorGeometry4b(value.x, value.y, value.z, value.w);
}

template<>
void mix_groups(const Span<ColorGeometry4b> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                MutableSpan<ColorGeometry4b> dst)
{
  mix_groups(src, groups, all_indices, byte_color_to_float4, float4_to_byte_color, dst);
}

template<>
void mix_groups(const Span<ColorGeometry4f> src,
                const OffsetIndices<int> all_groups,
                const Span<int> all_indices,
                MutableSpan<ColorGeometry4f> all_dst)
{
  mix_groups(src.cast<float4>(), all_groups, all_indices, all_dst.cast<float4>());
}

template<>
void mix_groups(const Span<float4x4> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                MutableSpan<float4x4> dst)
{
  for (const int dst_i : groups.index_range()) {
    float3 location_accum(0);
    float3 expmap_accum(0);
    float3 scale_accum(0);
    for (const int src_i : all_indices.slice(groups[dst_i])) {
      float3 location;
      math::Quaternion rotation;
      float3 scale;
      math::to_loc_rot_scale_safe<true>(src[src_i], location, rotation, scale);
      location_accum += location;
      expmap_accum += rotation.expmap();
      scale_accum += scale;
    }

    const float weight_inv = math::safe_rcp(float(groups[dst_i].size()));
    dst[dst_i] = math::from_loc_rot_scale<float4x4>(
        location_accum * weight_inv,
        math::Quaternion::expmap(expmap_accum * weight_inv),
        scale_accum * weight_inv);
  }
}

template<typename T>
void mix_groups(const Span<T> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                MutableSpan<T> dst)
{
  for (const int dst_i : dst.index_range()) {
    T accum(0);
    float weight_accum = 0.0f;
    for (const int i : groups[dst_i]) {
      const int src_i = all_indices[i];
      const float weight = all_weights[i];
      accum += src[src_i] * weight;
      weight_accum += weight;
    }
    dst[dst_i] = accum * math::safe_rcp(weight_accum);
  }
}

template<typename T, typename ToAccumFn, typename ToFinalFn>
void mix_groups(const Span<T> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                const ToAccumFn &to_accum_fn,
                const ToFinalFn &to_final_fn,
                MutableSpan<T> dst)
{
  using AccumT = std::invoke_result_t<ToAccumFn, T>;
  static_assert(std::is_same_v<std::invoke_result_t<ToFinalFn, AccumT>, T>);

  for (const int dst_i : dst.index_range()) {
    AccumT accum(0);
    float weight_accum = 0.0f;
    for (const int i : groups[dst_i]) {
      const int src_i = all_indices[i];
      const float weight = all_weights[i];
      accum += to_accum_fn(src[src_i]) * weight;
      weight_accum += weight;
    }
    dst[dst_i] = to_final_fn(accum * math::safe_rcp(weight_accum));
  }
}

template<>
void mix_groups(const Span<int> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                MutableSpan<int> dst)
{
  mix_groups(src, groups, all_indices, all_weights, int_to_double, double_to_int, dst);
}

template<>
void mix_groups(const Span<int2> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                MutableSpan<int2> dst)
{
  mix_groups(src, groups, all_indices, all_weights, int2_to_double2, double2_to_int2, dst);
}

template<>
void mix_groups(const Span<int8_t> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                MutableSpan<int8_t> dst)
{
  mix_groups(src, groups, all_indices, all_weights, int8_t_to_float, float_to_int8_t, dst);
}

template<>
void mix_groups(const Span<short2> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                MutableSpan<short2> dst)
{
  mix_groups(src, groups, all_indices, all_weights, short2_to_float2, float2_to_short2, dst);
}

template<>
void mix_groups(const Span<math::Quaternion> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                MutableSpan<math::Quaternion> dst)
{
  mix_groups(src, groups, all_indices, all_weights, quat_to_expmap, expmap_to_quat, dst);
}

template<>
void mix_groups(const Span<ColorGeometry4b> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                MutableSpan<ColorGeometry4b> dst)
{
  mix_groups(
      src, groups, all_indices, all_weights, byte_color_to_float4, float4_to_byte_color, dst);
}

template<>
void mix_groups(const Span<ColorGeometry4f> src,
                const OffsetIndices<int> all_groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                MutableSpan<ColorGeometry4f> all_dst)
{
  mix_groups(src.cast<float4>(), all_groups, all_indices, all_weights, all_dst.cast<float4>());
}

template<>
void mix_groups(const Span<float4x4> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                MutableSpan<float4x4> dst)
{
  for (const int dst_i : groups.index_range()) {
    float total_weight = 0.0f;
    float3 location_accum(0);
    float3 expmap_accum(0);
    float3 scale_accum(0);
    for (const int i : groups[dst_i]) {
      const int src_i = all_indices[i];
      const float weight = all_weights[i];
      float3 location;
      math::Quaternion rotation;
      float3 scale;
      math::to_loc_rot_scale_safe<true>(src[src_i], location, rotation, scale);
      location_accum += location * weight;
      expmap_accum += rotation.expmap() * weight;
      scale_accum += scale * weight;
      total_weight += weight;
    }

    const float weight_inv = math::safe_rcp(total_weight);
    dst[dst_i] = math::from_loc_rot_scale<float4x4>(
        location_accum * weight_inv,
        math::Quaternion::expmap(expmap_accum * weight_inv),
        scale_accum * weight_inv);
  }
}

template<>
void mix_groups(const Span<bool> src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const Span<float> all_weights,
                MutableSpan<bool> dst)
{
  for (const int dst_i : groups.index_range()) {
    const IndexRange group = groups[dst_i];
    dst[dst_i] = std::any_of(group.begin(), group.end(), [&](const int i) {
      return src[all_indices[i]] && all_weights[i] > 0.0f;
    });
  }
}

void mix_groups(const GSpan src,
                const OffsetIndices<int> groups,
                const Span<int> all_indices,
                const std::optional<Span<float>> all_weights,
                GMutableSpan dst)
{
  BLI_assert(groups.size() == dst.size());
  BLI_assert(groups.total_size() == all_indices.size());
  BLI_assert(!all_weights || groups.total_size() == all_weights->size());

  to_static_type(src.type(), [&]<typename T>() {
    if constexpr (!std::is_same_v<T, std::string>) {
      threading::parallel_for(
          groups.index_range(),
          2048,
          [&](const IndexRange range) {
            if (all_weights) {
              mix_groups(src.typed<T>(),
                         groups.slice(range),
                         all_indices,
                         *all_weights,
                         dst.typed<T>().slice(range));
            }
            else {
              mix_groups(
                  src.typed<T>(), groups.slice(range), all_indices, dst.typed<T>().slice(range));
            }
          },
          threading::accumulated_task_sizes(
              [&](const IndexRange range) { return groups[range].size(); }));
    }
  });
}

void gather(const GSpan src, const Span<int> map, GMutableSpan dst)
{
  gather(GVArray::from_span(src), map, IndexRange(dst.size()), dst);
}

void gather(const GVArray &src, const Span<int> map, GMutableSpan dst)
{
  gather(src, map, IndexRange(dst.size()), dst);
}

void gather(const GSpan src, const Span<int> map, const IndexMask &dst_mask, GMutableSpan dst)
{
  gather(GVArray::from_span(src), map, dst_mask, dst);
}

void gather(const GVArray &src, const Span<int> map, const IndexMask &dst_mask, GMutableSpan dst)
{
  to_static_type(src.type(), [&]<typename T>() {
    array_utils::gather(src.typed<T>(), map, dst_mask, dst.typed<T>());
  });
}

void gather_group_to_group(const OffsetIndices<int> src_offsets,
                           const OffsetIndices<int> dst_offsets,
                           const IndexMask &selection,
                           const GSpan src,
                           GMutableSpan dst)
{
  attribute_math::to_static_type(src.type(), [&]<typename T>() {
    array_utils::gather_group_to_group(
        src_offsets, dst_offsets, selection, src.typed<T>(), dst.typed<T>());
  });
}

void gather_ranges_to_groups(const Span<IndexRange> src_ranges,
                             const OffsetIndices<int> dst_offsets,
                             const GSpan src,
                             GMutableSpan dst)
{
  attribute_math::to_static_type(src.type(), [&]<typename T>() {
    Span<T> src_span = src.typed<T>();
    MutableSpan<T> dst_span = dst.typed<T>();

    threading::parallel_for(src_ranges.index_range(), 512, [&](const IndexRange range) {
      for (const int i : range) {
        dst_span.slice(dst_offsets[i]).copy_from(src_span.slice(src_ranges[i]));
      }
    });
  });
}

void gather_to_groups(const OffsetIndices<int> dst_offsets,
                      const IndexMask &src_selection,
                      const GSpan src,
                      GMutableSpan dst)
{
  attribute_math::to_static_type(src.type(), [&]<typename T>() {
    array_utils::gather_to_groups(dst_offsets, src_selection, src.typed<T>(), dst.typed<T>());
  });
}

}  // namespace blender::bke::attribute_math
