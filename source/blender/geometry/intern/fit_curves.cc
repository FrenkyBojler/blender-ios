/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_bounds.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task.hh"

#include "GEO_fit_curves.hh"

extern "C" {
#include "curve_fit_nd.h"
}

namespace blender::geometry {

bke::CurvesGeometry fit_curves(const Span<float3> positions,
                               const OffsetIndices<int> src_offsets,
                               const IndexMask &curve_selection,
                               const VArray<bool> &cyclic,
                               const VArray<float> &thresholds,
                               const FitMethod method,
                               Array<int> &r_old_to_new_map)
{
  const int dst_curves_num = curve_selection.size();
  /* Add one at the end so we can accumulate the sizes to offsets later. */
  Array<int> sizes_per_curve(dst_curves_num + 1);
  Array<int8_t> type_per_curve(dst_curves_num);
  Array<Vector<float3>> left_handles_per_curve(dst_curves_num);
  Array<Vector<float3>> control_points_per_curve(dst_curves_num);
  Array<Vector<float3>> right_handles_per_curve(dst_curves_num);
  Array<Vector<int>> old_to_new_per_curve(dst_curves_num);
  curve_selection.foreach_index(GrainSize(512), [&](const int64_t curve_i) {
    const IndexRange points = src_offsets[curve_i];
    const Span<float3> curve_positions = positions.slice(points);
    const bool use_cyclic = cyclic[curve_i];
    const float epsilon = thresholds[curve_i];

    Bounds<float3> bounds = *bounds::min_max(curve_positions);
    const float3 center = bounds.center();
    const float diagonal_distance = math::distance(bounds.min, bounds.max);

    Array<float3> normalized_positions(curve_positions.size());
    threading::parallel_for(curve_positions.index_range(), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        normalized_positions[i] = (curve_positions[i] - center) / diagonal_distance;
      }
    });

    const uint8_t flag = CURVE_FIT_CALC_HIGH_QUALIY | (use_cyclic) ? CURVE_FIT_CALC_CYCLIC : 0;

    float *r_cubic_array;
    uint32_t *r_orig_index_map;
    uint32_t r_cubic_array_len;
    uint32_t *r_corner_index_array;
    uint32_t r_corner_index_array_len;

    int error = 1;
    if (method == FitMethod::Split) {
      error = curve_fit_cubic_to_points_fl(normalized_positions.as_span().cast<float>().data(),
                                           normalized_positions.size(),
                                           3,
                                           epsilon,
                                           flag,
                                           nullptr,
                                           0,
                                           &r_cubic_array,
                                           &r_cubic_array_len,
                                           &r_orig_index_map,
                                           &r_corner_index_array,
                                           &r_corner_index_array_len);
    }
    else if (method == FitMethod::Refit) {
      error = curve_fit_cubic_to_points_refit_fl(
          normalized_positions.as_span().cast<float>().data(),
          normalized_positions.size(),
          3,
          epsilon,
          flag,
          nullptr,
          0,
          M_PI,
          &r_cubic_array,
          &r_cubic_array_len,
          &r_orig_index_map,
          &r_corner_index_array,
          &r_corner_index_array_len);
    }

    if (error) {
      /* Some error occured. Fall back to using the input positions as the (poly) curve. */
      sizes_per_curve[curve_i] = points.size();
      type_per_curve[curve_i] = CURVE_TYPE_POLY;
      control_points_per_curve[curve_i].resize(points.size());
      control_points_per_curve[curve_i].as_mutable_span().copy_from(curve_positions);
      return;
    }

    const int dst_points_num = r_cubic_array_len;
    const Span<float3> cubic_array_span(reinterpret_cast<float3 *>(r_cubic_array),
                                        dst_points_num * 3);
    const Span<int> orig_index_span(reinterpret_cast<int *>(r_orig_index_map), dst_points_num);

    sizes_per_curve[curve_i] = dst_points_num;
    type_per_curve[curve_i] = CURVE_TYPE_BEZIER;

    left_handles_per_curve[curve_i].resize(dst_points_num);
    control_points_per_curve[curve_i].resize(dst_points_num);
    right_handles_per_curve[curve_i].resize(dst_points_num);
    old_to_new_per_curve[curve_i].resize(dst_points_num);

    MutableSpan<float3> left_handles = left_handles_per_curve[curve_i].as_mutable_span();
    MutableSpan<float3> control_points = control_points_per_curve[curve_i].as_mutable_span();
    MutableSpan<float3> right_handles = right_handles_per_curve[curve_i].as_mutable_span();
    threading::parallel_for(IndexRange(dst_points_num), 4096, [&](const IndexRange range) {
      for (const int point_i : range) {
        const int index = point_i * 3;
        left_handles[point_i] = (cubic_array_span[index] * diagonal_distance) + center;
        control_points[point_i] = (cubic_array_span[index + 1] * diagonal_distance) + center;
        right_handles[point_i] = (cubic_array_span[index + 2] * diagonal_distance) + center;
      }
    });

    old_to_new_per_curve[curve_i].as_mutable_span().copy_from(orig_index_span);
  });

  const OffsetIndices points_by_curve = offset_indices::accumulate_counts_to_offsets(
      sizes_per_curve.as_mutable_span());

  bke::CurvesGeometry dst_curves(points_by_curve.total_size(), dst_curves_num);
  dst_curves.offsets_for_write().copy_from(points_by_curve.data());

  dst_curves.curve_types_for_write().copy_from(type_per_curve);
  dst_curves.update_curve_types();

  cyclic.materialize_to_uninitialized(dst_curves.cyclic_for_write());

  dst_curves.handle_types_left_for_write().fill(BEZIER_HANDLE_ALIGN);
  dst_curves.handle_types_right_for_write().fill(BEZIER_HANDLE_ALIGN);

  r_old_to_new_map.reinitialize(dst_curves.points_num());

  MutableSpan<float3> handle_positions_left = dst_curves.handle_positions_left_for_write();
  MutableSpan<float3> control_point_positions = dst_curves.positions_for_write();
  MutableSpan<float3> handle_positions_right = dst_curves.handle_positions_right_for_write();
  threading::parallel_for(dst_curves.curves_range(), 4096, [&](const IndexRange range) {
    for (const int curve_i : range) {
      const IndexRange points = points_by_curve[curve_i];
      handle_positions_left.slice(points).copy_from(left_handles_per_curve[curve_i].as_span());
      control_point_positions.slice(points).copy_from(control_points_per_curve[curve_i].as_span());
      handle_positions_right.slice(points).copy_from(right_handles_per_curve[curve_i].as_span());

      r_old_to_new_map.as_mutable_span().slice(points).copy_from(
          old_to_new_per_curve[curve_i].as_span());
    }
  });

  return dst_curves;
}

}  // namespace blender::geometry
