/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_offset_indices.hh"
#include "BLI_task.hh"

#include "GEO_fit_curves_to_points.hh"

extern "C" {
#include "curve_fit_nd.h"
}

namespace blender::geometry {

bke::CurvesGeometry fit_curves_to_points(const Span<float3> positions,
                                         const OffsetIndices<int> points_by_curve,
                                         const Span<bool> cyclic,
                                         const float epsilon,
                                         Array<int> &r_new_to_old_map)
{
  const int num_curves = points_by_curve.size();
  Array<int> curve_sizes(num_curves);
  threading::parallel_for(points_by_curve.index_range(), 512, [&](const IndexRange range) {
    for (const int curve_i : range) {
      const IndexRange curve_range = points_by_curve[curve_i];
      const Span<float3> points = positions.slice(curve_range);
      const bool use_cyclic = cyclic[curve_i];

      const uint8_t flag = CURVE_FIT_CALC_HIGH_QUALIY | (use_cyclic) ? CURVE_FIT_CALC_CYCLIC : 0;

      float *r_cubic_array;
      uint32_t *r_orig_index_map;
      uint32_t r_cubic_array_len;
      if (curve_fit_cubic_to_points_refit_fl(*points.data(),
                                             points.size(),
                                             3,
                                             epsilon,
                                             flag,
                                             nullptr,
                                             0,
                                             0.0f,
                                             &r_cubic_array,
                                             &r_cubic_array_len,
                                             &r_orig_index_map,
                                             nullptr,
                                             nullptr))
      {
        /* Some error occured. Fall back to using the input positions as the curve. */
        curve_sizes[curve_i] = points.size();
        continue;
      }

      const Span<float3> cubic_array_span(reinterpret_cast<float3 *>(r_cubic_array),
                                          r_cubic_array_len * 3);
      curve_sizes[curve_i] = cubic_array_span.size();
    }
  });
  return {};
}

}  // namespace blender::geometry
