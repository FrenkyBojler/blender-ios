/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"

#include "BKE_curves_utils.hh"
#include "BKE_deform.hh"

#include "GEO_fit_curves.hh"

namespace blender {

extern "C" {
#include "curve_fit_nd.h"
}

namespace geometry {

static std::optional<int> attr_type_dimensions(const bke::AttrType type)
{
  switch (type) {
    case bke::AttrType::Float:
      return 1;
    case bke::AttrType::Float2:
      return 2;
    case bke::AttrType::Float3:
      return 3;
    case bke::AttrType::ColorFloat:
      return 4;
    case bke::AttrType::Bool:
    case bke::AttrType::Int8:
    case bke::AttrType::Int16_2D:
    case bke::AttrType::Int32:
    case bke::AttrType::Int32_2D:
    case bke::AttrType::Float4x4:
    case bke::AttrType::ColorByte:
    case bke::AttrType::Quaternion:
    case bke::AttrType::String:
      return {};
    default:
      return {};
  }
}

bke::CurvesGeometry fit_poly_curve_attributes_to_bezier_curves(
    const bke::CurvesGeometry &src_curves,
    const IndexMask &curve_selection,
    const Span<GMutableSpan> attributes,
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

  /* Add one for the positions attribute and one for the final offset. */
  Array<int> num_dimensions_per_attribute(attributes.size() + 1 + 1);
  num_dimensions_per_attribute[0] = 3;
  for (const int i : attributes.index_range()) {
    const GSpan attribute = attributes[i];
    BLI_assert(attribute.size() == src_curves.points_num());

    const bke::AttrType type = bke::cpp_type_to_attribute_type(attribute.type());
    const std::optional<int> dimensions = attr_type_dimensions(type);
    BLI_assert_msg(dimensions.has_value(), "Unexpected attribute type!");
    if (!dimensions.has_value()) {
      /* Fall back to source curves. */
      return src_curves;
    }
    num_dimensions_per_attribute[i + 1] = dimensions.value();
  }
  const OffsetIndices src_points_by_curve = src_curves.offsets();
  const Span<float3> src_positions = src_curves.positions();
  const VArray<bool> src_cyclic = src_curves.cyclic();

  const OffsetIndices dimensions_by_attribute = offset_indices::accumulate_counts_to_offsets(
      num_dimensions_per_attribute.as_mutable_span());
  const int stride = dimensions_by_attribute.total_size();
  Array<float> attribute_data(stride * src_curves.points_num());

  /* The curve fitting library expects the data to be in an "array of structs" format rather than
   * "structs of arrays". Converts the layout below.
   * Note that the flat array uses a size thats larger than the selection to allow easy computation
   * of offset indices. This means that part of the array might be left uninitialized.*/
  auto write_interleaved_attribute_data = [&](const GSpan src_attribute,
                                              const IndexRange dimensions) {
    curve_selection.foreach_index(GrainSize(1024), [&](const int64_t curve_i) {
      threading::parallel_for(src_points_by_curve[curve_i], 8192, [&](const IndexRange range) {
        for (const int point : range) {
          const Span<float> values(static_cast<const float *>(src_attribute[point]),
                                   dimensions.size());
          for (const int dim_i : dimensions.index_range()) {
            const int dim = dimensions[dim_i];
            const int index = point * stride + dim;
            attribute_data[index] = values[dim_i];
          }
        }
      });
    });
  };

  /* The positions are always used when doing the curve fitting. */
  write_interleaved_attribute_data(src_positions, dimensions_by_attribute[0]);
  /* Add all additional (optional) dimensions. */
  for (const int i : attributes.index_range()) {
    const GSpan attribute = attributes[i];
    const IndexRange dimensions = dimensions_by_attribute[i + 1];
    write_interleaved_attribute_data(attribute, dimensions);
  }

  bke::CurvesGeometry dst_curves = bke::curves::copy_only_curve_domain(src_curves);
  BKE_defgroup_copy_list(&dst_curves.vertex_group_names, &src_curves.vertex_group_names);

  IndexMaskMemory memory;
  const IndexMask unselected_curves = curve_selection.complement(src_curves.curves_range(),
                                                                 memory);

  /* Write the new sizes to the dst_curve_sizes, they will be accumulated later to offsets. */
  MutableSpan<int> dst_curve_sizes = dst_curves.offsets_for_write();
  offset_indices::copy_group_sizes(src_points_by_curve, unselected_curves, dst_curve_sizes);
  MutableSpan<int8_t> dst_curve_types = dst_curves.curve_types_for_write();

  /* NOTE: These spans own the data from the curve fit C-API. */
  Array<MutableSpan<float>> cubic_array_per_curve(curve_selection.size());
  Array<MutableSpan<int>> corner_indices_per_curve(curve_selection.size());
  Array<MutableSpan<int>> original_indices_per_curve(curve_selection.size());

  bool success = false;
  curve_selection.foreach_index(GrainSize(128), [&](const int64_t curve_i, const int64_t pos) {
    const IndexRange points = src_points_by_curve[curve_i];
    if (points.size() < 2) {
      dst_curve_sizes[curve_i] = points.size();
      dst_curve_types[curve_i] = CURVE_TYPE_POLY;
      return;
    }
    const Span<float> curve_attribute_data = attribute_data.as_span().slice(
        points.start() * stride, points.size() * stride);
    const bool is_cyclic = src_cyclic[curve_i];
    const float epsilon = thresholds[curve_i];

    IndexMaskMemory memory;
    const IndexMask corner_mask =
        IndexMask::from_bools(points, corners, memory).shift(-points.start(), memory);
    Array<int> src_corners(corner_mask.size());
    corner_mask.to_indices(src_corners.as_mutable_span());
    const uint *src_corners_ptr = src_corners.is_empty() ?
                                      nullptr :
                                      reinterpret_cast<const uint *>(src_corners.data());

    const uint8_t flag = CURVE_FIT_CALC_HIGH_QUALITY | ((is_cyclic) ? CURVE_FIT_CALC_CYCLIC : 0);

    float *cubic_array = nullptr;
    uint32_t *orig_index_map = nullptr;
    uint32_t cubic_array_size = 0;
    uint32_t *corner_index_array = nullptr;
    uint32_t corner_index_array_size = 0;
    int error = 1;
    if (method == FitMethod::Split) {
      error = curve_fit_cubic_to_points_fl(curve_attribute_data.data(),
                                           points.size(),
                                           stride,
                                           epsilon,
                                           flag,
                                           src_corners_ptr,
                                           src_corners.size(),
                                           &cubic_array,
                                           &cubic_array_size,
                                           &orig_index_map,
                                           &corner_index_array,
                                           &corner_index_array_size);
    }
    else if (method == FitMethod::Refit) {
      error = curve_fit_cubic_to_points_refit_fl(curve_attribute_data.data(),
                                                 points.size(),
                                                 stride,
                                                 epsilon,
                                                 flag,
                                                 src_corners_ptr,
                                                 src_corners.size(),
                                                 /* Don't use automatic corner detection. */
                                                 FLT_MAX,
                                                 &cubic_array,
                                                 &cubic_array_size,
                                                 &orig_index_map,
                                                 &corner_index_array,
                                                 &corner_index_array_size);
    }

    if (error) {
      /* Some error occurred. Fall back to using the input positions as the (poly) curve. */
      dst_curve_sizes[curve_i] = points.size();
      dst_curve_types[curve_i] = CURVE_TYPE_POLY;
      return;
    }

    success = true;

    const int dst_points_num = cubic_array_size;
    BLI_assert(dst_points_num > 0);

    dst_curve_sizes[curve_i] = dst_points_num;
    dst_curve_types[curve_i] = CURVE_TYPE_BEZIER;

    cubic_array_per_curve[pos] = MutableSpan<float>(reinterpret_cast<float *>(cubic_array),
                                                    dst_points_num * 3 * stride);
    corner_indices_per_curve[pos] = MutableSpan<int>(reinterpret_cast<int *>(corner_index_array),
                                                     corner_index_array_size);
    original_indices_per_curve[pos] = MutableSpan<int>(reinterpret_cast<int *>(orig_index_map),
                                                       dst_points_num);
  });

  if (!success) {
    /* None of the curve fittings succeeded. */
    return src_curves;
  }

  const OffsetIndices dst_points_by_curve = offset_indices::accumulate_counts_to_offsets(
      dst_curve_sizes);
  dst_curves.resize(dst_curves.offsets().last(), dst_curves.curves_num());

  const std::optional<Span<float3>> src_handles_left = src_curves.handle_positions_left();
  const std::optional<Span<float3>> src_handles_right = src_curves.handle_positions_right();
  const VArraySpan<int8_t> src_handle_types_left = src_curves.handle_types_left();
  const VArraySpan<int8_t> src_handle_types_right = src_curves.handle_types_right();

  MutableSpan<float3> dst_positions = dst_curves.positions_for_write();
  MutableSpan<float3> dst_handles_left = dst_curves.handle_positions_left_for_write();
  MutableSpan<float3> dst_handles_right = dst_curves.handle_positions_right_for_write();
  MutableSpan<int8_t> dst_handle_types_left = dst_curves.handle_types_left_for_write();
  MutableSpan<int8_t> dst_handle_types_right = dst_curves.handle_types_right_for_write();

  /* First handle the unselected curves. */
  if (src_handles_left) {
    array_utils::copy_group_to_group(src_points_by_curve,
                                     dst_points_by_curve,
                                     unselected_curves,
                                     *src_handles_left,
                                     dst_handles_left);
  }
  array_utils::copy_group_to_group(
      src_points_by_curve, dst_points_by_curve, unselected_curves, src_positions, dst_positions);
  if (src_handles_right) {
    array_utils::copy_group_to_group(src_points_by_curve,
                                     dst_points_by_curve,
                                     unselected_curves,
                                     *src_handles_right,
                                     dst_handles_right);
  }
  if (!src_handle_types_left.is_empty()) {
    array_utils::copy_group_to_group(src_points_by_curve,
                                     dst_points_by_curve,
                                     unselected_curves,
                                     src_handle_types_left,
                                     dst_handle_types_left);
  }
  if (!src_handle_types_right.is_empty()) {
    array_utils::copy_group_to_group(src_points_by_curve,
                                     dst_points_by_curve,
                                     unselected_curves,
                                     src_handle_types_right,
                                     dst_handle_types_right);
  }

  Array<int> old_by_new_map(dst_curves.points_num());
  unselected_curves.foreach_index(GrainSize(1024), [&](const int64_t curve_i) {
    const IndexRange src_points = src_points_by_curve[curve_i];
    const IndexRange dst_points = dst_points_by_curve[curve_i];
    array_utils::fill_index_range<int>(old_by_new_map.as_mutable_span().slice(dst_points),
                                       src_points.start());
  });

  /* Now copy the data of the newly fitted curves. */
  curve_selection.foreach_index(GrainSize(1024), [&](const int64_t curve_i, const int64_t pos) {
    const IndexRange src_points = src_points_by_curve[curve_i];
    const IndexRange dst_points = dst_points_by_curve[curve_i];
    MutableSpan<float3> positions = dst_positions.slice(dst_points);
    MutableSpan<int> old_by_new = old_by_new_map.as_mutable_span().slice(dst_points);

    if (dst_curve_types[curve_i] == CURVE_TYPE_POLY) {
      /* Handle the curves for which the curve fitting has failed. */
      BLI_assert(src_points.size() == dst_points.size());
      positions.copy_from(src_positions.slice(src_points));
      dst_handles_left.slice(dst_points).copy_from(src_positions.slice(src_points));
      dst_handles_right.slice(dst_points).copy_from(src_positions.slice(src_points));
      dst_handle_types_left.slice(dst_points).fill(BEZIER_HANDLE_FREE);
      dst_handle_types_right.slice(dst_points).fill(BEZIER_HANDLE_FREE);
      array_utils::fill_index_range<int>(old_by_new, src_points.start());
      return;
    }

    /* Only extract the position attribute from the cubic array. Discard the other attribute
     * dimensions. */
    const Span<float> cubic_array = cubic_array_per_curve[pos];
    BLI_assert(dst_points.size() * 3 * stride == cubic_array.size());
    MutableSpan<float3> left_handles = dst_handles_left.slice(dst_points);
    MutableSpan<float3> right_handles = dst_handles_right.slice(dst_points);
    threading::parallel_for(dst_points.index_range(), 8192, [&](const IndexRange range) {
      for (const int i : range) {
        const int index = i * 3;
        positions[i] = &cubic_array[(index + 1) * stride];
        left_handles[i] = &cubic_array[index * stride];
        right_handles[i] = &cubic_array[(index + 2) * stride];
      }
    });

    const Span<int> corner_indices = corner_indices_per_curve[pos];
    dst_handle_types_left.slice(dst_points).fill(BEZIER_HANDLE_ALIGN);
    dst_handle_types_right.slice(dst_points).fill(BEZIER_HANDLE_ALIGN);
    dst_handle_types_left.slice(dst_points).fill_indices(corner_indices, BEZIER_HANDLE_FREE);
    dst_handle_types_right.slice(dst_points).fill_indices(corner_indices, BEZIER_HANDLE_FREE);

    const Span<int> original_indices = original_indices_per_curve[pos];
    threading::parallel_for(dst_points.index_range(), 8192, [&](const IndexRange range) {
      for (const int i : range) {
        old_by_new[i] = src_points[original_indices[i]];
      }
    });
  });

  dst_curves.update_curve_types();

  bke::gather_attributes(
      src_curves.attributes(),
      bke::AttrDomain::Point,
      bke::AttrDomain::Point,
      bke::attribute_filter_with_skip_ref(
          attribute_filter,
          {"position", "handle_left", "handle_right", "handle_type_left", "handle_type_right"}),
      old_by_new_map,
      dst_curves.attributes_for_write());

  /* Free all the data from the C-API
   * Note: This data is allocated inside the library and has to be freed with `free`. */
  for (MutableSpan<float> cubic_array : cubic_array_per_curve) {
    free(cubic_array.data());
  }
  for (MutableSpan<int> corner_indices : corner_indices_per_curve) {
    free(corner_indices.data());
  }
  for (MutableSpan<int> original_indices : original_indices_per_curve) {
    free(original_indices.data());
  }

  return dst_curves;
}

bke::CurvesGeometry fit_poly_to_bezier_curves(const bke::CurvesGeometry &src_curves,
                                              const IndexMask &curve_selection,
                                              const VArray<float> &thresholds,
                                              const VArray<bool> &corners,
                                              const FitMethod method,
                                              const bke::AttributeFilter &attribute_filter)
{
  return fit_poly_curve_attributes_to_bezier_curves(
      src_curves, curve_selection, {}, thresholds, corners, method, attribute_filter);
}

}  // namespace geometry
}  // namespace blender
