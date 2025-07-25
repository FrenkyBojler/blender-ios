/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "BLI_generic_span.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

namespace blender::geometry::fmm {

template<typename Func> inline void to_static_type(const CPPType &type, const Func &func)
{
  type.to_static_type_tag<float, float3>([&](auto type_tag) {
    using T = typename decltype(type_tag)::type;
    if constexpr (!std::is_same_v<T, void>) {
      func(T());
    }
    else {
      BLI_assert_unreachable();
    }
  });
}

inline float minimal_dinstance_to_claster(const float radius,
                                          const int degree,
                                          const float error_value)
{
  BLI_assert(error_value > 1.0f);
  /**
   * Total sum of points function in cluster have to be least than difference between minimal and
   * maximal possible results of sampling inside of the cluster separately * total number of points
   * - 1. So approximation will always be around actual result. Centre of the claster bounds with
   * some points too near and far to the sampler location:
   *
   * 1 / (#distance + #radius) <= 1 / (#distance - #radius).
   *
   * They are equal at ~infinite distance. But with error they can be treat as equal much near
   * To approximate this use some factor (1 <= #error_value <= infinite) to say how large error is
   * acceptable:
   *
   * 1 / (#distance + #radius) >= 1 / (#distance - #radius) * #error_value.
   *
   * Version in arbitrary degree of the distance to each point:
   *
   * (1 / (#distance + #radius)) ^ #degree >= #error_value * (1 / (#distance - #radius)) ^ #degree.
   *
   *
   * Solution for such a minimum distance to to bounds in an acceptable error range:
   * */
  const float precision_root = math::pow<float>(error_value, math::rcp<float>(degree));
  return -((1.0f + precision_root) / (1.0f - precision_root) * radius);
}

void akdbh_accumulate_in(OffsetIndices<int> buckets_offsets,
                         int total_depth,
                         Span<float> src_joints_min_distance,
                         Span<float3> src_joints_centre,
                         Span<Span<float>> src_joints_value,
                         std::array<Span<float>, 3> src_bucket_position,
                         Span<Span<float>> src_bucket_value,
                         int power_value,
                         float offset_value,
                         std::array<Span<float>, 3> sample_position,
                         Span<MutableSpan<float>> dst_buckets_data,
                         std::optional<IndexRange> sampler_to_bucket_range = std::nullopt);

}  // namespace blender::geometry::fmm

namespace blender {

void transpose(const Span<float3> src, Span<MutableSpan<float>> dst);

void transpose(const Span<Span<float>> src, MutableSpan<float3> dst);

void transpose_gather(const Span<float3> src,
                      const Span<int> indices,
                      Span<MutableSpan<float>> dst);

void transpose_gather(const Span<Span<float>> src,
                      const Span<int> indices,
                      MutableSpan<float3> dst);

void transpose_gather(const Span<float3> src, const IndexMask mask, Span<MutableSpan<float>> dst);

void transpose_gather(const Span<Span<float>> src, const IndexMask mask, MutableSpan<float3> dst);

void transpose_scatter(const Span<float3> src,
                       const Span<int> indices,
                       Span<MutableSpan<float>> dst);

void transpose_scatter(const Span<Span<float>> src,
                       const Span<int> indices,
                       MutableSpan<float3> dst);

void transpose_scatter(const Span<float3> src, const IndexMask mask, Span<MutableSpan<float>> dst);

void transpose_scatter(const Span<Span<float>> src, const IndexMask mask, MutableSpan<float3> dst);

}  // namespace blender
