/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_bounds.hh"
#include "BLI_task.hh"

#include "GEO_fit_curves.hh"

extern "C" {
#include "curve_fit_nd.h"
}

namespace blender::geometry {

bke::CurvesGeometry fit_curves(const Span<float3> positions,
                               const OffsetIndices<int> src_offsets,
                               const IndexMask &curve_selection,
                               const VArray<float> &thresholds,
                               const VArray<bool> &corners,
                               const VArray<bool> &cyclic,
                               const FitMethod method,
                               Array<int> &r_old_to_new_map)
{
  const int dst_curves_num = curve_selection.size();
  BLI_assert(src_offsets.total_size() == corners.size());
  Array<bool> is_corner(src_offsets.total_size(), false);
  if (!corners.is_single() || corners.get_internal_single() == true) {
    IndexMaskMemory memory;
    const IndexMask point_selection = IndexMask::from_ranges(src_offsets, curve_selection, memory);
    corners.materialize(point_selection, is_corner.as_mutable_span());
  }
  /* Add one at the end so we can accumulate the sizes to offsets later. */
  Array<int> sizes_per_curve(dst_curves_num + 1);
  Array<int8_t> type_per_curve(dst_curves_num);
  Array<Vector<float3>> left_handles_per_curve(dst_curves_num);
  Array<Vector<float3>> control_points_per_curve(dst_curves_num);
  Array<Vector<float3>> right_handles_per_curve(dst_curves_num);
  Array<Vector<int8_t>> left_handle_type_per_curve(dst_curves_num);
  Array<Vector<int8_t>> right_handle_type_per_curve(dst_curves_num);
  Array<Vector<int>> old_to_new_per_curve(dst_curves_num);
  curve_selection.foreach_index(GrainSize(512), [&](const int64_t curve_i) {
    const IndexRange points = src_offsets[curve_i];
    const Span<float3> curve_positions = positions.slice(points);
    const bool use_cyclic = cyclic[curve_i];
    const float epsilon = thresholds[curve_i];

    IndexMaskMemory memory;
    const IndexMask src_corner_mask = IndexMask::from_bools(is_corner.as_span().slice(points),
                                                            memory);
    /* Both curve fitting algorithms expect the first and last points for non-cyclic curves to be
     * treated as if they were corners. */
    const bool use_first_as_corner = !use_cyclic && !src_corner_mask.contains(0);
    const bool use_last_as_corner = !use_cyclic &&
                                    !src_corner_mask.contains(points.index_range().last());
    Array<int> src_corner_indices;
    if (!src_corner_mask.is_empty()) {
      src_corner_indices.reinitialize(src_corner_mask.size() + int(use_first_as_corner) +
                                      int(use_last_as_corner));
      if (use_first_as_corner) {
        src_corner_indices.first() = 0;
      }
      src_corner_mask.to_indices(src_corner_indices.as_mutable_span()
                                     .drop_front(use_first_as_corner ? 1 : 0)
                                     .drop_back(use_last_as_corner ? 1 : 0));
      if (use_last_as_corner) {
        src_corner_indices.last() = points.index_range().last();
      }
    }
    const uint *src_indices_corner_ptr = !src_corner_indices.is_empty() ?
                                             reinterpret_cast<uint *>(src_corner_indices.data()) :
                                             nullptr;

    const uint8_t flag = CURVE_FIT_CALC_HIGH_QUALIY | (use_cyclic) ? CURVE_FIT_CALC_CYCLIC : 0;

    float *r_cubic_array;
    uint32_t *r_orig_index_map;
    uint32_t r_cubic_array_len;
    uint32_t *r_corner_index_array;
    uint32_t r_corner_index_array_len;

    int error = 1;
    if (method == FitMethod::Split) {
      error = curve_fit_cubic_to_points_fl(curve_positions.cast<float>().data(),
                                           curve_positions.size(),
                                           3,
                                           epsilon,
                                           flag,
                                           src_indices_corner_ptr,
                                           src_corner_indices.size(),
                                           &r_cubic_array,
                                           &r_cubic_array_len,
                                           &r_orig_index_map,
                                           &r_corner_index_array,
                                           &r_corner_index_array_len);
    }
    else if (method == FitMethod::Refit) {
      error = curve_fit_cubic_to_points_refit_fl(curve_positions.cast<float>().data(),
                                                 curve_positions.size(),
                                                 3,
                                                 epsilon,
                                                 flag,
                                                 src_indices_corner_ptr,
                                                 src_corner_indices.size(),
                                                 /* Don't use automatic corner detection. */
                                                 FLT_MAX,
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
    const Span<int> dst_corner_indices(reinterpret_cast<int *>(r_corner_index_array),
                                       r_corner_index_array_len);
    const Span<int> orig_indices_map(reinterpret_cast<int *>(r_orig_index_map), dst_points_num);

    sizes_per_curve[curve_i] = dst_points_num;
    type_per_curve[curve_i] = CURVE_TYPE_BEZIER;

    left_handles_per_curve[curve_i].resize(dst_points_num);
    control_points_per_curve[curve_i].resize(dst_points_num);
    right_handles_per_curve[curve_i].resize(dst_points_num);
    left_handle_type_per_curve[curve_i].resize(dst_points_num);
    right_handle_type_per_curve[curve_i].resize(dst_points_num);
    old_to_new_per_curve[curve_i].resize(dst_points_num);

    MutableSpan<float3> left_handles = left_handles_per_curve[curve_i].as_mutable_span();
    MutableSpan<float3> control_points = control_points_per_curve[curve_i].as_mutable_span();
    MutableSpan<float3> right_handles = right_handles_per_curve[curve_i].as_mutable_span();
    threading::parallel_for(IndexRange(dst_points_num), 4096, [&](const IndexRange range) {
      for (const int point_i : range) {
        const int index = point_i * 3;
        left_handles[point_i] = cubic_array_span[index];
        control_points[point_i] = cubic_array_span[index + 1];
        right_handles[point_i] = cubic_array_span[index + 2];
      }
    });

    MutableSpan<int8_t> left_handle_types = left_handle_type_per_curve[curve_i].as_mutable_span();
    MutableSpan<int8_t> right_handle_types =
        right_handle_type_per_curve[curve_i].as_mutable_span();
    if (!dst_corner_indices.is_empty()) {
      const IndexMask dst_corner_mask = IndexMask::from_indices(dst_corner_indices, memory);
      index_mask::masked_fill(left_handle_types, int8_t(BEZIER_HANDLE_FREE), dst_corner_mask);
      index_mask::masked_fill(right_handle_types, int8_t(BEZIER_HANDLE_FREE), dst_corner_mask);
    }
    else {
      left_handle_types.fill(BEZIER_HANDLE_ALIGN);
      right_handle_types.fill(BEZIER_HANDLE_ALIGN);
    }

    old_to_new_per_curve[curve_i].as_mutable_span().copy_from(orig_indices_map);
  });

  const OffsetIndices points_by_curve = offset_indices::accumulate_counts_to_offsets(
      sizes_per_curve.as_mutable_span());

  bke::CurvesGeometry dst_curves(points_by_curve.total_size(), dst_curves_num);
  dst_curves.offsets_for_write().copy_from(points_by_curve.data());

  dst_curves.curve_types_for_write().copy_from(type_per_curve);

  r_old_to_new_map.reinitialize(dst_curves.points_num());

  MutableSpan<float3> handle_positions_left = dst_curves.handle_positions_left_for_write();
  MutableSpan<float3> control_point_positions = dst_curves.positions_for_write();
  MutableSpan<float3> handle_positions_right = dst_curves.handle_positions_right_for_write();
  MutableSpan<int8_t> handle_types_left = dst_curves.handle_types_left_for_write();
  MutableSpan<int8_t> handle_types_right = dst_curves.handle_types_right_for_write();
  threading::parallel_for(dst_curves.curves_range(), 4096, [&](const IndexRange range) {
    for (const int curve_i : range) {
      const IndexRange points = points_by_curve[curve_i];
      handle_positions_left.slice(points).copy_from(left_handles_per_curve[curve_i].as_span());
      control_point_positions.slice(points).copy_from(control_points_per_curve[curve_i].as_span());
      handle_positions_right.slice(points).copy_from(right_handles_per_curve[curve_i].as_span());
      handle_types_left.slice(points).copy_from(left_handle_type_per_curve[curve_i].as_span());
      handle_types_right.slice(points).copy_from(right_handle_type_per_curve[curve_i].as_span());

      r_old_to_new_map.as_mutable_span().slice(points).copy_from(
          old_to_new_per_curve[curve_i].as_span());
    }
  });
  dst_curves.update_curve_types();

  return dst_curves;
}

bke::CurvesGeometry fit_curves(const bke::CurvesGeometry &src_curves,
                               const IndexMask &curve_selection,
                               const VArray<float> &thresholds,
                               const VArray<bool> &corners,
                               const FitMethod method,
                               const bke::AttributeFilter &attribute_filter)
{
  Array<int> old_to_new_map;
  bke::CurvesGeometry curves = geometry::fit_curves(src_curves.positions(),
                                                    src_curves.points_by_curve(),
                                                    curve_selection,
                                                    thresholds,
                                                    corners,
                                                    src_curves.cyclic(),
                                                    method,
                                                    old_to_new_map);

  bke::gather_attributes(
      src_curves.attributes(),
      bke::AttrDomain::Point,
      bke::AttrDomain::Point,
      bke::attribute_filter_with_skip_ref(
          attribute_filter,
          {"position", "handle_type_left", "handle_type_right", "handle_left", "handle_right"}),
      old_to_new_map,
      curves.attributes_for_write());
  bke::gather_attributes(src_curves.attributes(),
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_with_skip_ref(attribute_filter, {"curve_type"}),
                         curve_selection,
                         curves.attributes_for_write());
  return curves;
}

}  // namespace blender::geometry
