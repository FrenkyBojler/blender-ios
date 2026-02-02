/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"

#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_deform.hh"

#include "GEO_offset_curves.hh"

namespace blender::geometry {

bke::CurvesGeometry offset_curves(const bke::CurvesGeometry &src_curves,
                                  const Span<float3> normals,
                                  const IndexMask &curve_selection,
                                  const OffsetCornerType corner_type,
                                  const float offset_distance,
                                  const float miter_angle)
{
  const OffsetIndices src_points_by_curve = src_curves.offsets();
  const VArray<bool> src_cyclic = src_curves.cyclic();
  const Span<float3> src_positions = src_curves.positions();
  const Span<float3> src_handles_left = *src_curves.handle_positions_left();
  const Span<float3> src_handles_right = *src_curves.handle_positions_right();
  const VArray<int8_t> src_curve_types = src_curves.curve_types();

  const float miter_limit = cos(miter_angle);

  IndexMaskMemory memory;
  const IndexMask unselected_curves = curve_selection.complement(src_curves.curves_range(),
                                                                 memory);

  bke::CurvesGeometry dst_curves = bke::curves::copy_only_curve_domain(src_curves);
  BKE_defgroup_copy_list(&dst_curves.vertex_group_names, &src_curves.vertex_group_names);

  MutableSpan<int> dst_curve_sizes = dst_curves.offsets_for_write();
  offset_indices::copy_group_sizes(src_points_by_curve, unselected_curves, dst_curve_sizes);

  Vector<float3> offset_pos;
  Vector<float3> offset_handles_left;
  Vector<float3> offset_handles_right;
  Vector<int> offset_old_by_new_map;

  curve_selection.foreach_index(GrainSize(1024), [&](const int64_t curve_i) {
    const IndexRange src_points = src_points_by_curve[curve_i];
    Span<float3> src_pos = src_positions.slice(src_points);
    const int8_t curve_type = src_curve_types[curve_i];
    const bool cyclic = src_cyclic[curve_i];

    const int offset_pos_size = offset_pos.size();
    const float3 plane_norm = normals[curve_i];

    for (const int i : src_pos.index_range()) {
      float3 A = src_pos[(i - 1 + src_pos.size()) % src_pos.size()];
      const float3 B = src_pos[i];
      float3 C = src_pos[(i + 1) % src_pos.size()];

      if (curve_type == CURVE_TYPE_BEZIER) {
        A = src_handles_left[src_points[i]];
        C = src_handles_right[src_points[i]];
      }

      const float3 BA = math::normalize(B - A);
      const float3 CB = math::normalize(C - B);

      const float3 BA_tan = math::normalize(math::cross(BA, plane_norm));
      const float3 CB_tan = math::normalize(math::cross(CB, plane_norm));

      const float cos_theta = math::dot(BA_tan, CB_tan);

      float3 offset = float3(0.0);

      if (!cyclic && (i == 0 || i == src_pos.size() - 1)) {
        if (i == 0) {
          offset = CB_tan * offset_distance;
        }
        if (i == src_pos.size() - 1) {
          offset = BA_tan * offset_distance;
        }
      }
      else {
        if (corner_type == OffsetCornerType::Miter && cos_theta < miter_limit) {
          const float sin_theta = sqrt(1 - cos_theta * cos_theta);
          const float S = (sqrt(2 * (cos_theta + 1)) - cos_theta - 1) / sin_theta;

          const float3 norm_dir = math::normalize(BA + CB);
          const float3 tan_dir = math::normalize(BA_tan + CB_tan);

          const float3 pos_1 = B + (tan_dir - norm_dir * S) * offset_distance;
          const float3 pos_2 = B + (tan_dir + norm_dir * S) * offset_distance;
          offset_pos.append(pos_1);
          offset_pos.append(pos_2);
          offset_old_by_new_map.append(src_points[i]);
          offset_old_by_new_map.append(src_points[i]);

          if (!src_handles_left.is_empty()) {
            /* TODO: Use a better approximation. */
            offset_handles_left.append(
                (A - B) * (1.0f + math::safe_divide(offset_distance, 1.0f + cos_theta)) + pos_1);

            offset_handles_right.append(pos_1 + (pos_2 - pos_1) / 3.0f);
            offset_handles_left.append(pos_2 + (pos_1 - pos_2) / 3.0f);

            offset_handles_right.append(
                (C - B) * (1.0f + math::safe_divide(offset_distance, 1.0f + cos_theta)) + pos_2);
          }

          continue;
        }

        offset = math::safe_divide(BA_tan + CB_tan, 1.0f + cos_theta) * offset_distance;
      }

      offset_pos.append(B + offset);
      offset_old_by_new_map.append(src_points[i]);

      if (!src_handles_left.is_empty()) {
        /* TODO: Use a better approximation. */
        offset_handles_left.append(
            (A - B) * (1.0f + math::safe_divide(offset_distance, 1.0f + cos_theta)) + B + offset);
        offset_handles_right.append(
            (C - B) * (1.0f + math::safe_divide(offset_distance, 1.0f + cos_theta)) + B + offset);
      }
    }

    dst_curve_sizes[curve_i] = offset_pos.size() - offset_pos_size;
  });

  const OffsetIndices dst_points_by_curve = offset_indices::accumulate_counts_to_offsets(
      dst_curve_sizes);
  dst_curves.resize(dst_curves.offsets().last(), dst_curves.curves_num());

  Array<int> old_by_new_map(dst_curves.points_num());
  unselected_curves.foreach_index(GrainSize(1024), [&](const int64_t curve_i) {
    const IndexRange src_points = src_points_by_curve[curve_i];
    const IndexRange dst_points = dst_points_by_curve[curve_i];
    array_utils::fill_index_range<int>(old_by_new_map.as_mutable_span().slice(dst_points),
                                       src_points.start());
  });

  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();
  const bke::AttributeAccessor src_attributes = src_curves.attributes();

  bke::copy_attributes(
      src_attributes, bke::AttrDomain::Curve, bke::AttrDomain::Curve, {}, dst_attributes);
  array_utils::copy(src_cyclic, dst_curves.cyclic_for_write());

  MutableSpan<float3> dst_positions = dst_curves.positions_for_write();
  MutableSpan<float3> dst_handles_left = dst_curves.handle_positions_left_for_write();
  MutableSpan<float3> dst_handles_right = dst_curves.handle_positions_right_for_write();

  array_utils::copy_group_to_group(
      src_points_by_curve, dst_points_by_curve, unselected_curves, src_positions, dst_positions);
  if (!src_handles_left.is_empty()) {
    array_utils::copy_group_to_group(src_points_by_curve,
                                     dst_points_by_curve,
                                     unselected_curves,
                                     src_handles_left,
                                     dst_handles_left);
    array_utils::copy_group_to_group(src_points_by_curve,
                                     dst_points_by_curve,
                                     unselected_curves,
                                     src_handles_right,
                                     dst_handles_right);
  }

  int index = 0;
  curve_selection.foreach_index(GrainSize(1024), [&](const int64_t curve_i) {
    const IndexRange dst_points = dst_points_by_curve[curve_i];
    const IndexRange off_points = dst_points.index_range().shift(index);
    MutableSpan<int> map_range = old_by_new_map.as_mutable_span().slice(dst_points);
    map_range.copy_from(offset_old_by_new_map.as_span().slice(off_points));

    dst_positions.slice(dst_points).copy_from(offset_pos.as_span().slice(off_points));
    if (!offset_handles_left.is_empty()) {
      dst_handles_left.slice(dst_points)
          .copy_from(offset_handles_left.as_span().slice(off_points));
      dst_handles_right.slice(dst_points)
          .copy_from(offset_handles_right.as_span().slice(off_points));
    }
    index += dst_points.size();
  });

  bke::gather_attributes(
      src_curves.attributes(),
      bke::AttrDomain::Point,
      bke::AttrDomain::Point,
      bke::attribute_filter_with_skip_ref({}, {"position", "handle_left", "handle_right"}),
      old_by_new_map,
      dst_curves.attributes_for_write());

  dst_curves.update_curve_types();

  return dst_curves;
}

}  // namespace blender::geometry
