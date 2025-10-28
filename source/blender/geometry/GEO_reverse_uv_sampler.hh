/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <optional>

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"

namespace blender::geometry {

/**
 * Can find the polygon/triangle that maps to a specific uv coordinate.
 *
 * \note this uses a trivial implementation currently that has to be replaced.
 */
class ReverseUVSampler {
 public:
  struct LookupGrid;

 private:
  Span<float2> uv_map_;
  Span<int3> corner_tris_;
  Array<float2> centers_;
  float2 resolution_;
  std::unique_ptr<LookupGrid> lookup_grid_;
  int64_t live_count_;

 public:
  ReverseUVSampler(Span<float2> uv_map,
                   Span<int3> corner_tris,
                   std::optional<Bounds<float2>> known_uv_bounds = std::nullopt,
                   std::optional<int64_t> samples_num_hint = std::nullopt);
  ~ReverseUVSampler();

  enum class ResultType {
    None,
    Ok,
    Multiple,
  };

  struct Result {
    ResultType type = ResultType::None;
    int tri_index = -1;
    float3 bary_weights;
  };

  Result sample(const float2 &query_uv) const;
  void sample_many(Span<float2> query_uvs, MutableSpan<Result> r_results) const;
};

}  // namespace blender::geometry
