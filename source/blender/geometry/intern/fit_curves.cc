/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_bounds.hh"
#include "BLI_task.hh"

#include "BKE_curves_utils.hh"

#include "GEO_fit_curves.hh"

extern "C" {
#include "curve_fit_nd.h"
}

namespace blender::geometry {

bke::CurvesGeometry fit_curves(const bke::CurvesGeometry &src_curves,
                               const IndexMask &curve_selection,
                               const VArray<float> &thresholds,
                               const VArray<bool> &corners,
                               const FitMethod method,
                               const bke::AttributeFilter &attribute_filter)
{
  if (curve_selection.is_empty()) {
    return src_curves;
  }

  BLI_assert(thresholds.size() == src_curves.curves_num());
  BLI_assert(corners.size() == src_curves.points_num());

  const OffsetIndices src_offsets = src_curves.offsets();
  const Span<float3> src_positions = src_curves.positions();
  const VArray<bool> src_cyclic = src_curves.cyclic();

  /* Build boolean array that marks all the corners. */
  Array<bool> is_corner(src_curves.points_num(), false);
  if (!corners.is_single() || corners.get_internal_single() == true) {
    IndexMaskMemory memory;
    const IndexMask point_selection = IndexMask::from_ranges(src_offsets, curve_selection, memory);
    corners.materialize(point_selection, is_corner.as_mutable_span());
  }

  IndexMaskMemory memory;
  const IndexMask unselected_curves = curve_selection.complement(src_curves.curves_range(),
                                                                 memory);

  /* Add one at the end so we can accumulate the sizes to offsets later. */
  Array<int> all_curve_sizes(src_curves.curves_num() + 1);
  offset_indices::copy_group_sizes(
      src_offsets, unselected_curves, all_curve_sizes.as_mutable_span());
  Array<int8_t> all_curve_types(src_curves.curves_num());
  src_curves.curve_types().materialize(all_curve_types.as_mutable_span());

  Array<Vector<float3>> left_handles_per_curve(curve_selection.size());
  Array<Vector<float3>> control_points_per_curve(curve_selection.size());
  Array<Vector<float3>> right_handles_per_curve(curve_selection.size());
  Array<Vector<int8_t>> left_handle_type_per_curve(curve_selection.size());
  Array<Vector<int8_t>> right_handle_type_per_curve(curve_selection.size());
  Array<Vector<int>> old_to_new_per_curve(curve_selection.size());

  std::atomic<bool> success = false;
  curve_selection.foreach_index(GrainSize(512), [&](const int64_t curve_i, const int64_t pos) {
    const IndexRange points = src_offsets[curve_i];
    const Span<float3> curve_positions = src_positions.slice(points);
    const bool use_cyclic = src_cyclic[curve_i];
    const float epsilon = thresholds[curve_i];

    IndexMaskMemory corner_memory;
    const IndexMask src_corner_mask = IndexMask::from_bools(is_corner.as_span().slice(points),
                                                            corner_memory);
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

    const uint8_t flag = CURVE_FIT_CALC_HIGH_QUALIY | ((use_cyclic) ? CURVE_FIT_CALC_CYCLIC : 0);

    float *r_cubic_array = nullptr;
    uint32_t *r_orig_index_map = nullptr;
    uint32_t r_cubic_array_len = 0;
    uint32_t *r_corner_index_array = nullptr;
    uint32_t r_corner_index_array_len = 0;

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
      all_curve_sizes[curve_i] = points.size();
      all_curve_types[curve_i] = CURVE_TYPE_POLY;

      control_points_per_curve[pos].resize(points.size());
      control_points_per_curve[pos].as_mutable_span().copy_from(curve_positions);

      old_to_new_per_curve[pos].resize(points.size());
      array_utils::fill_index_range<int>(old_to_new_per_curve[pos].as_mutable_span(),
                                         points.start());
      return;
    }
    success.store(true, std::memory_order_relaxed);
    BLI_assert(r_cubic_array != nullptr && r_cubic_array_len > 0);

    const int dst_points_num = r_cubic_array_len;
    const Span<float3> cubic_array_span(reinterpret_cast<float3 *>(r_cubic_array),
                                        dst_points_num * 3);
    const Span<int> dst_corner_indices(reinterpret_cast<int *>(r_corner_index_array),
                                       r_corner_index_array_len);
    const Span<int> orig_indices_map(reinterpret_cast<int *>(r_orig_index_map), dst_points_num);

    all_curve_sizes[curve_i] = dst_points_num;
    all_curve_types[curve_i] = CURVE_TYPE_BEZIER;

    left_handles_per_curve[pos].resize(dst_points_num);
    control_points_per_curve[pos].resize(dst_points_num);
    right_handles_per_curve[pos].resize(dst_points_num);
    left_handle_type_per_curve[pos].resize(dst_points_num, BEZIER_HANDLE_ALIGN);
    right_handle_type_per_curve[pos].resize(dst_points_num, BEZIER_HANDLE_ALIGN);
    old_to_new_per_curve[pos].resize(dst_points_num);

    MutableSpan<float3> left_handles = left_handles_per_curve[pos].as_mutable_span();
    MutableSpan<float3> control_points = control_points_per_curve[pos].as_mutable_span();
    MutableSpan<float3> right_handles = right_handles_per_curve[pos].as_mutable_span();
    threading::parallel_for(IndexRange(dst_points_num), 8192, [&](const IndexRange range) {
      for (const int point_i : range) {
        const int index = point_i * 3;
        left_handles[point_i] = cubic_array_span[index];
        control_points[point_i] = cubic_array_span[index + 1];
        right_handles[point_i] = cubic_array_span[index + 2];
      }
    });

    MutableSpan<int8_t> left_handle_types = left_handle_type_per_curve[pos].as_mutable_span();
    MutableSpan<int8_t> right_handle_types = right_handle_type_per_curve[pos].as_mutable_span();
    if (!dst_corner_indices.is_empty()) {
      const IndexMask dst_corner_mask = IndexMask::from_indices(dst_corner_indices, corner_memory);
      index_mask::masked_fill(left_handle_types, int8_t(BEZIER_HANDLE_FREE), dst_corner_mask);
      index_mask::masked_fill(right_handle_types, int8_t(BEZIER_HANDLE_FREE), dst_corner_mask);
    }

    MutableSpan<int> old_by_new = old_to_new_per_curve[pos].as_mutable_span();
    threading::parallel_for(IndexRange(dst_points_num), 8192, [&](const IndexRange range) {
      for (const int i : range) {
        old_by_new[i] = orig_indices_map[i] + points.start();
      }
    });
  });

  if (!success) {
    /* None of the curve fittings succeeded. */
    return src_curves;
  }

  /* Create new curves geometry. Copy only the curve domain from the src_curves. */
  bke::CurvesGeometry dst_curves = bke::curves::copy_only_curve_domain(src_curves);
  const OffsetIndices dst_points_by_curve = offset_indices::accumulate_counts_to_offsets(
      all_curve_sizes.as_mutable_span());
  dst_curves.offsets_for_write().copy_from(dst_points_by_curve.data());
  dst_curves.resize(dst_curves.offsets().last(), dst_curves.curves_num());

  const Span<float3> src_handle_positions_left = src_curves.handle_positions_left();
  const Span<float3> src_control_point_positions = src_curves.positions();
  const Span<float3> src_handle_positions_right = src_curves.handle_positions_right();
  const VArraySpan<int8_t> src_handle_types_left = src_curves.handle_types_left();
  const VArraySpan<int8_t> src_handle_types_right = src_curves.handle_types_right();

  Array<int> old_to_new_map(dst_curves.points_num());
  MutableSpan<float3> dst_handle_positions_left = dst_curves.handle_positions_left_for_write();
  MutableSpan<float3> dst_control_point_positions = dst_curves.positions_for_write();
  MutableSpan<float3> dst_handle_positions_right = dst_curves.handle_positions_right_for_write();
  MutableSpan<int8_t> dst_handle_types_left = dst_curves.handle_types_left_for_write();
  MutableSpan<int8_t> dst_handle_types_right = dst_curves.handle_types_right_for_write();

  /* First handle the unselected curves. */
  if (!src_handle_positions_left.is_empty()) {
    array_utils::copy_group_to_group(src_offsets,
                                     dst_points_by_curve,
                                     unselected_curves,
                                     src_handle_positions_left,
                                     dst_handle_positions_left);
  }
  array_utils::copy_group_to_group(src_offsets,
                                   dst_points_by_curve,
                                   unselected_curves,
                                   src_control_point_positions,
                                   dst_control_point_positions);
  if (!src_handle_positions_right.is_empty()) {
    array_utils::copy_group_to_group(src_offsets,
                                     dst_points_by_curve,
                                     unselected_curves,
                                     src_handle_positions_right,
                                     dst_handle_positions_right);
  }
  if (!src_handle_types_left.is_empty()) {
    array_utils::copy_group_to_group(src_offsets,
                                     dst_points_by_curve,
                                     unselected_curves,
                                     src_handle_types_left,
                                     dst_handle_types_left);
  }
  if (!src_handle_types_right.is_empty()) {
    array_utils::copy_group_to_group(src_offsets,
                                     dst_points_by_curve,
                                     unselected_curves,
                                     src_handle_types_right,
                                     dst_handle_types_right);
  }
  unselected_curves.foreach_index(GrainSize(1024), [&](const int64_t curve_i) {
    const IndexRange src_points = src_offsets[curve_i];
    const IndexRange dst_points = dst_points_by_curve[curve_i];
    array_utils::fill_index_range<int>(old_to_new_map.as_mutable_span().slice(dst_points),
                                       src_points.start());
  });

  /* Now copy the data of the newly fitted curves. */
  curve_selection.foreach_index(GrainSize(1024), [&](const int64_t curve_i, const int64_t pos) {
    const IndexRange dst_points = dst_points_by_curve[curve_i];

    dst_control_point_positions.slice(dst_points)
        .copy_from(control_points_per_curve[pos].as_span());
    old_to_new_map.as_mutable_span()
        .slice(dst_points)
        .copy_from(old_to_new_per_curve[pos].as_span());

    if (all_curve_types[curve_i] != CURVE_TYPE_BEZIER) {
      /* Skip handles for when the curve fitting failed for some reason. */
      return;
    }

    dst_handle_positions_left.slice(dst_points).copy_from(left_handles_per_curve[pos].as_span());
    dst_handle_positions_right.slice(dst_points).copy_from(right_handles_per_curve[pos].as_span());
    dst_handle_types_left.slice(dst_points).copy_from(left_handle_type_per_curve[pos].as_span());
    dst_handle_types_right.slice(dst_points).copy_from(right_handle_type_per_curve[pos].as_span());
  });

  dst_curves.curve_types_for_write().copy_from(all_curve_types);
  dst_curves.update_curve_types();

  dst_curves.tag_topology_changed();

  bke::gather_attributes(
      src_curves.attributes(),
      bke::AttrDomain::Point,
      bke::AttrDomain::Point,
      bke::attribute_filter_with_skip_ref(
          attribute_filter,
          {"position", "handle_left", "handle_right", "handle_type_left", "handle_type_right"}),
      old_to_new_map,
      dst_curves.attributes_for_write());

  return dst_curves;
}

}  // namespace blender::geometry
