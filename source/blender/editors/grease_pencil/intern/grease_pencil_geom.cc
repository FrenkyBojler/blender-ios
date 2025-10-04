/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include <algorithm>

#include "BLI_array_utils.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_kdtree.h"
#include "BLI_lasso_2d.hh"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_rect.h"
#include "BLI_sort.hh"
#include "BLI_stack.hh"
#include "BLI_task.hh"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_grease_pencil.hh"

#include "DNA_curves_types.h"
#include "DNA_gpencil_legacy_types.h"

#include "ED_curves.hh"
#include "ED_grease_pencil.hh"
#include "ED_view3d.hh"

#include "GEO_merge_curves.hh"

extern "C" {
#include "curve_fit_nd.h"
}

namespace blender::ed::greasepencil {

int64_t ramer_douglas_peucker_simplify(
    const IndexRange range,
    const float epsilon,
    const FunctionRef<float(int64_t, int64_t, int64_t)> dist_function,
    MutableSpan<bool> points_to_delete)
{
  /* Mark all points to not be removed. */
  points_to_delete.slice(range).fill(false);
  int64_t total_points_to_remove = 0;

  Stack<IndexRange> stack;
  stack.push(range);
  while (!stack.is_empty()) {
    const IndexRange sub_range = stack.pop();
    /* Skip ranges with less than 3 points. All points are kept. */
    if (sub_range.size() < 3) {
      continue;
    }
    const IndexRange inside_range = sub_range.drop_front(1).drop_back(1);
    /* Compute the maximum distance and the corresponding index. */
    float max_dist = -1.0f;
    int max_index = -1;
    for (const int64_t index : inside_range) {
      const float dist = dist_function(sub_range.first(), sub_range.last(), index);
      if (dist > max_dist) {
        max_dist = dist;
        max_index = index - sub_range.first();
      }
    }

    if (max_dist > epsilon) {
      /* Found point outside the epsilon-sized strip. The point at `max_index` will be kept, repeat
       * the search on the left & right side. */
      stack.push(sub_range.slice(0, max_index + 1));
      stack.push(sub_range.slice(max_index, sub_range.size() - max_index));
    }
    else {
      /* Points in `sub_range` are inside the epsilon-sized strip. Mark them to be deleted. */
      total_points_to_remove += inside_range.size();
      points_to_delete.slice(inside_range).fill(true);
    }
  }
  return total_points_to_remove;
}

Array<float2> polyline_fit_curve(Span<float2> points,
                                 const float error_threshold,
                                 const IndexMask &corner_mask)
{
  if (points.is_empty()) {
    return {};
  }
  double total_length = 0.0;
  for (const int point_i : points.index_range().drop_front(1)) {
    total_length += math::distance(points[point_i - 1], points[point_i]);
  }
  /* Just return a dot. */
  if (total_length < 1e-8) {
    return Array<float2>({points[0], points[0], points[0]});
  }

  Array<int32_t> indices(corner_mask.size());
  corner_mask.to_indices(indices.as_mutable_span());
  uint *indicies_ptr = corner_mask.is_empty() ? nullptr : reinterpret_cast<uint *>(indices.data());

  float *cubic_array;
  uint cubic_array_len;
  int error = curve_fit_cubic_to_points_fl(*points.data(),
                                           points.size(),
                                           2,
                                           error_threshold,
                                           CURVE_FIT_CALC_HIGH_QUALIY,
                                           indicies_ptr,
                                           indices.size(),
                                           &cubic_array,
                                           &cubic_array_len,
                                           nullptr,
                                           nullptr,
                                           nullptr);

  if (error != 0) {
    /* Some error occurred. Return. */
    return {};
  }

  if (cubic_array == nullptr) {
    return {};
  }

  Span<float2> cubic_array_span(reinterpret_cast<float2 *>(cubic_array), cubic_array_len * 3);
  Array<float2> curve_positions(cubic_array_span);
  /* Free the c-style array. */
  free(cubic_array);
  return curve_positions;
}

IndexMask polyline_detect_corners(Span<float2> points,
                                  const float radius_min,
                                  const float radius_max,
                                  const int samples_max,
                                  const float angle_threshold,
                                  IndexMaskMemory &memory)
{
  if (points.is_empty()) {
    return {};
  }
  if (points.size() == 1) {
    return IndexMask::from_indices<int>({0}, memory);
  }
  uint *corners;
  uint corners_len;
  const int error = curve_fit_corners_detect_fl(*points.data(),
                                                points.size(),
                                                float2::type_length,
                                                radius_min,
                                                radius_max,
                                                samples_max,
                                                angle_threshold,
                                                &corners,
                                                &corners_len);
  if (error != 0) {
    /* Error occurred, return. */
    return IndexMask();
  }

  if (corners == nullptr) {
    return IndexMask();
  }

  BLI_assert(samples_max < std::numeric_limits<int>::max());
  Span<int> indices(reinterpret_cast<int *>(corners), corners_len);
  const IndexMask corner_mask = IndexMask::from_indices<int>(indices, memory);
  /* Free the c-style array. */
  free(corners);
  return corner_mask;
}

int curve_merge_by_distance(const IndexRange points,
                            const Span<float> distances,
                            const IndexMask &selection,
                            const float merge_distance,
                            MutableSpan<int> r_merge_indices)
{
  /* We use a KDTree_1d here, because we can only merge neighboring points in the curves. */
  KDTree_1d *tree = BLI_kdtree_1d_new(selection.size());
  /* The selection is an IndexMask of the points just in this curve. */
  selection.foreach_index_optimized<int64_t>([&](const int64_t i, const int64_t pos) {
    BLI_kdtree_1d_insert(tree, pos, &distances[i - points.first()]);
  });
  BLI_kdtree_1d_balance(tree);

  Array<int> selection_merge_indices(selection.size(), -1);
  const int duplicate_count = BLI_kdtree_1d_calc_duplicates_fast(
      tree, merge_distance, false, selection_merge_indices.data());
  BLI_kdtree_1d_free(tree);

  array_utils::fill_index_range<int>(r_merge_indices);

  selection.foreach_index([&](const int src_index, const int pos) {
    const int merge_index = selection_merge_indices[pos];
    if (merge_index != -1) {
      const int src_merge_index = selection[merge_index] - points.first();
      r_merge_indices[src_index - points.first()] = src_merge_index;
    }
  });

  return duplicate_count;
}

blender::bke::CurvesGeometry curves_merge_by_distance(const bke::CurvesGeometry &src_curves,
                                                      const float merge_distance,
                                                      const IndexMask &selection,
                                                      const bke::AttributeFilter &attribute_filter)
{
  /* NOTE: The code here is an adapted version of #blender::geometry::point_merge_by_distance. */

  const int src_point_size = src_curves.points_num();
  if (src_point_size == 0) {
    return {};
  }
  const OffsetIndices<int> points_by_curve = src_curves.points_by_curve();
  const VArray<bool> cyclic = src_curves.cyclic();
  src_curves.ensure_evaluated_lengths();

  bke::CurvesGeometry dst_curves = bke::curves::copy_only_curve_domain(src_curves);
  MutableSpan<int> dst_offsets = dst_curves.offsets_for_write();

  std::atomic<int> total_duplicate_count = 0;
  Array<Array<int>> merge_indices_per_curve(src_curves.curves_num());
  threading::parallel_for(src_curves.curves_range(), 512, [&](const IndexRange range) {
    for (const int curve_i : range) {
      const IndexRange points = points_by_curve[curve_i];
      merge_indices_per_curve[curve_i].reinitialize(points.size());

      Array<float> distances_along_curve(points.size() + int(cyclic[curve_i]));
      distances_along_curve.first() = 0.0f;
      const Span<float> lengths = src_curves.evaluated_lengths_for_curve(curve_i, cyclic[curve_i]);
      distances_along_curve.as_mutable_span().drop_front(1).copy_from(lengths);

      MutableSpan<int> merge_indices = merge_indices_per_curve[curve_i].as_mutable_span();
      array_utils::fill_index_range<int>(merge_indices);

      const int duplicate_count = curve_merge_by_distance(points,
                                                          distances_along_curve,
                                                          selection.slice_content(points),
                                                          merge_distance,
                                                          merge_indices);
      /* Write the curve size. The counts will be accumulated to offsets below. */
      dst_offsets[curve_i] = points.size() - duplicate_count;
      total_duplicate_count += duplicate_count;
    }
  });

  const int dst_point_size = src_point_size - total_duplicate_count;
  dst_curves.resize(dst_point_size, src_curves.curves_num());
  offset_indices::accumulate_counts_to_offsets(dst_offsets);

  int merged_points = 0;
  Array<int> src_to_dst_indices(src_point_size);
  for (const int curve_i : src_curves.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    const Span<int> merge_indices = merge_indices_per_curve[curve_i].as_span();
    for (const int i : points.index_range()) {
      const int point_i = points.start() + i;
      src_to_dst_indices[point_i] = point_i - merged_points;
      if (merge_indices[i] != i) {
        merged_points++;
      }
    }
  }

  Array<int> point_merge_counts(dst_point_size, 0);
  for (const int curve_i : src_curves.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    const Span<int> merge_indices = merge_indices_per_curve[curve_i].as_span();
    for (const int i : points.index_range()) {
      const int merge_index = merge_indices[i];
      const int point_src = points.start() + merge_index;
      const int dst_index = src_to_dst_indices[point_src];
      point_merge_counts[dst_index]++;
    }
  }

  Array<int> map_offsets_data(dst_point_size + 1);
  map_offsets_data.as_mutable_span().drop_back(1).copy_from(point_merge_counts);
  OffsetIndices<int> map_offsets = offset_indices::accumulate_counts_to_offsets(map_offsets_data);

  point_merge_counts.fill(0);

  Array<int> merge_map_indices(src_point_size);
  for (const int curve_i : src_curves.curves_range()) {
    const IndexRange points = points_by_curve[curve_i];
    const Span<int> merge_indices = merge_indices_per_curve[curve_i].as_span();
    for (const int i : points.index_range()) {
      const int point_i = points.start() + i;
      const int merge_index = merge_indices[i];
      const int dst_index = src_to_dst_indices[points.start() + merge_index];
      merge_map_indices[map_offsets[dst_index].first() + point_merge_counts[dst_index]] = point_i;
      point_merge_counts[dst_index]++;
    }
  }

  bke::AttributeAccessor src_attributes = src_curves.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();
  src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (attribute_filter.allow_skip(iter.name)) {
      return;
    }
    if (iter.domain != bke::AttrDomain::Point) {
      return;
    }

    bke::GAttributeReader src_attribute = iter.get();
    bke::attribute_math::convert_to_static_type(src_attribute.varray.type(), [&](auto dummy) {
      using T = decltype(dummy);
      if constexpr (!std::is_void_v<bke::attribute_math::DefaultMixer<T>>) {
        bke::SpanAttributeWriter<T> dst_attribute =
            dst_attributes.lookup_or_add_for_write_only_span<T>(iter.name, bke::AttrDomain::Point);
        BLI_assert(dst_attribute);
        VArraySpan<T> src = src_attribute.varray.typed<T>();

        threading::parallel_for(dst_curves.points_range(), 1024, [&](IndexRange range) {
          for (const int dst_point_i : range) {
            /* Create a separate mixer for every point to avoid allocating temporary buffers
             * in the mixer the size of the result curves and to improve memory locality. */
            bke::attribute_math::DefaultMixer<T> mixer{dst_attribute.span.slice(dst_point_i, 1)};

            Span<int> src_merge_indices = merge_map_indices.as_span().slice(
                map_offsets[dst_point_i]);
            for (const int src_point_i : src_merge_indices) {
              mixer.mix_in(0, src[src_point_i]);
            }

            mixer.finalize();
          }
        });

        dst_attribute.finish();
      }
    });
  });

  if (dst_curves.nurbs_has_custom_knots()) {
    bke::curves::nurbs::update_custom_knot_modes(
        dst_curves.curves_range(), NURBS_KNOT_MODE_NORMAL, NURBS_KNOT_MODE_NORMAL, dst_curves);
  }
  return dst_curves;
}

bke::CurvesGeometry curves_merge_endpoints_by_distance(
    const ARegion &region,
    const bke::CurvesGeometry &src_curves,
    const float4x4 &layer_to_world,
    const float merge_distance,
    const IndexMask &selection,
    const bke::AttributeFilter &attribute_filter)
{
  const OffsetIndices src_points_by_curve = src_curves.points_by_curve();
  const Span<float3> src_positions = src_curves.positions();
  const float merge_distance_squared = merge_distance * merge_distance;

  Array<float2> screen_start_points(src_curves.curves_num());
  Array<float2> screen_end_points(src_curves.curves_num());
  const VArray<bool> cyclic = *src_curves.attributes().lookup_or_default<bool>(
      "cyclic", bke::AttrDomain::Curve, false);
  /* For comparing screen space positions use a 2D KDTree. Each curve adds 2 points. */
  KDTree_2d *tree = BLI_kdtree_2d_new(2 * src_curves.curves_num());

  threading::parallel_for(src_curves.curves_range(), 1024, [&](const IndexRange range) {
    for (const int src_i : range) {
      const IndexRange points = src_points_by_curve[src_i];
      const float3 start_pos = src_positions[points.first()];
      const float3 end_pos = src_positions[points.last()];
      const float3 start_world = math::transform_point(layer_to_world, start_pos);
      const float3 end_world = math::transform_point(layer_to_world, end_pos);

      ED_view3d_project_float_global(
          &region, start_world, screen_start_points[src_i], V3D_PROJ_TEST_NOP);
      ED_view3d_project_float_global(
          &region, end_world, screen_end_points[src_i], V3D_PROJ_TEST_NOP);
    }
  });
  /* Note: KDTree insertion is not thread-safe, don't parallelize this. */
  for (const int src_i : src_curves.curves_range()) {
    if (cyclic[src_i] == true) {
      continue;
    }
    BLI_kdtree_2d_insert(tree, src_i * 2, screen_start_points[src_i]);
    BLI_kdtree_2d_insert(tree, src_i * 2 + 1, screen_end_points[src_i]);
  }
  BLI_kdtree_2d_balance(tree);

  Array<int> connect_to_curve(src_curves.curves_num(), -1);
  Array<bool> flip_direction(src_curves.curves_num(), false);
  selection.foreach_index(GrainSize(512), [&](const int src_i) {
    const float2 &start_co = screen_start_points[src_i];
    const float2 &end_co = screen_end_points[src_i];
    /* Index of KDTree points so they can be ignored. */
    const int start_index = src_i * 2;
    const int end_index = src_i * 2 + 1;

    KDTreeNearest_2d nearest_start, nearest_end;
    const bool is_start_ok = (BLI_kdtree_2d_find_nearest_cb_cpp(
                                  tree,
                                  start_co,
                                  &nearest_start,
                                  [&](const int other, const float * /*co*/, const float dist_sq) {
                                    if (start_index == other || dist_sq > merge_distance_squared) {
                                      return 0;
                                    }
                                    return 1;
                                  }) != -1);
    const bool is_end_ok = (BLI_kdtree_2d_find_nearest_cb_cpp(
                                tree,
                                end_co,
                                &nearest_end,
                                [&](const int other, const float * /*co*/, const float dist_sq) {
                                  if (end_index == other || dist_sq > merge_distance_squared) {
                                    return 0;
                                  }
                                  return 1;
                                }) != -1);

    if (is_start_ok) {
      const int curve_index = nearest_start.index / 2;
      const bool is_end_point = bool(nearest_start.index % 2);
      if (connect_to_curve[curve_index] < 0) {
        connect_to_curve[curve_index] = src_i;
        flip_direction[curve_index] = !is_end_point;
      }
    }
    if (is_end_ok) {
      const int curve_index = nearest_end.index / 2;
      const bool is_end_point = bool(nearest_end.index % 2);
      if (connect_to_curve[src_i] < 0) {
        connect_to_curve[src_i] = curve_index;
        flip_direction[curve_index] = is_end_point;
      }
    }
  });
  BLI_kdtree_2d_free(tree);

  return geometry::curves_merge_endpoints(
      src_curves, connect_to_curve, flip_direction, attribute_filter);
}

/* Generate a full circle around a point. */
static void generate_circle_from_point(const float3 &pt,
                                       const float radius,
                                       const int corner_subdivisions,
                                       const int src_point_index,
                                       Vector<float3> &r_perimeter,
                                       Vector<int> &r_src_indices)
{
  /* Number of points is 2^(n+2) on a full circle (n=corner_subdivisions). */
  BLI_assert(corner_subdivisions >= 0);
  const int num_points = 1 << (corner_subdivisions + 2);
  const float delta_angle = 2 * M_PI / float(num_points);
  const float delta_cos = math::cos(delta_angle);
  const float delta_sin = math::sin(delta_angle);

  float3 vec = float3(radius, 0, 0);
  for ([[maybe_unused]] const int i : IndexRange(num_points)) {
    r_perimeter.append(pt + vec);
    r_src_indices.append(src_point_index);

    const float x = delta_cos * vec.x - delta_sin * vec.y;
    const float y = delta_sin * vec.x + delta_cos * vec.y;
    vec = float3(x, y, 0.0f);
  }
}

/* Generate points in an counter-clockwise arc between two directions. */
static void generate_arc_from_point_to_point(const float3 &from,
                                             const float3 &to,
                                             const float3 &center_pt,
                                             const int corner_subdivisions,
                                             const int src_point_index,
                                             Vector<float3> &r_perimeter,
                                             Vector<int> &r_src_indices)
{
  const float3 vec_from = from - center_pt;
  const float3 vec_to = to - center_pt;
  if (math::is_zero(vec_from) || math::is_zero(vec_to)) {
    r_perimeter.append(center_pt);
    r_src_indices.append(src_point_index);
    return;
  }

  const float cos_angle = math::dot(vec_from.xy(), vec_to.xy());
  const float sin_angle = vec_from.x * vec_to.y - vec_from.y * vec_to.x;
  /* Compute angle in range [0, 2pi) so that the rotation is always counter-clockwise. */
  const float angle = math::atan2(-sin_angle, -cos_angle) + M_PI;

  /* Number of points is 2^(n+1) + 1 on half a circle (n=corner_subdivisions)
   * so we multiply by (angle / pi) to get the right amount of
   * points to insert. */
  const int num_full = (1 << (corner_subdivisions + 1)) + 1;
  const int num_points = num_full * math::abs(angle) / M_PI;
  if (num_points < 2) {
    r_perimeter.append(center_pt + vec_from);
    r_src_indices.append(src_point_index);
    return;
  }
  const float delta_angle = angle / float(num_points - 1);
  const float delta_cos = math::cos(delta_angle);
  const float delta_sin = math::sin(delta_angle);

  float3 vec = vec_from;
  for ([[maybe_unused]] const int i : IndexRange(num_points)) {
    r_perimeter.append(center_pt + vec);
    r_src_indices.append(src_point_index);

    const float x = delta_cos * vec.x - delta_sin * vec.y;
    const float y = delta_sin * vec.x + delta_cos * vec.y;
    vec = float3(x, y, 0.0f);
  }
}

/* Generate a semi-circle around a point, opposite the direction. */
static void generate_cap(const float3 &point,
                         const float3 &tangent,
                         const float radius,
                         const int corner_subdivisions,
                         const eGPDstroke_Caps cap_type,
                         const int src_point_index,
                         Vector<float3> &r_perimeter,
                         Vector<int> &r_src_indices)
{
  const float3 normal = math::normalize(float3{tangent.y, -tangent.x, 0.0f});
  switch (cap_type) {
    case GP_STROKE_CAP_ROUND:
      generate_arc_from_point_to_point(point - normal * radius,
                                       point + normal * radius,
                                       point,
                                       corner_subdivisions,
                                       src_point_index,
                                       r_perimeter,
                                       r_src_indices);
      break;
    case GP_STROKE_CAP_FLAT:
      r_perimeter.append(point - normal * radius);
      r_src_indices.append(src_point_index);
      r_perimeter.append(point + normal * radius);
      r_src_indices.append(src_point_index);
      break;
    case GP_STROKE_CAP_MAX:
      BLI_assert_unreachable();
      break;
  }
}

/* Generate a corner between two segments, using `miter_limit_angle` as the corner type.
 * NOTE: The perimeter is considered to be to the right hand side of the stroke. The left side
 * perimeter can be generated by reversing the order of points. */
static void generate_corner(const float3 &pt_a,
                            const float3 &pt_b,
                            const float3 &pt_c,
                            const float radius,
                            const float miter_limit_angle,
                            const int corner_subdivisions,
                            const int src_point_index,
                            Vector<float3> &r_perimeter,
                            Vector<int> &r_src_indices)
{
  const float length = math::length(pt_c - pt_b);
  const float length_prev = math::length(pt_b - pt_a);
  const float2 tangent = math::normalize((pt_c - pt_b).xy());
  const float2 tangent_prev = math::normalize((pt_b - pt_a).xy());
  const float3 normal = {tangent.y, -tangent.x, 0.0f};
  const float3 normal_prev = {tangent_prev.y, -tangent_prev.x, 0.0f};

  const float sin_angle = tangent_prev.x * tangent.y - tangent_prev.y * tangent.x;
  /* Whether the corner is an inside or outside corner.
   * This determines whether an arc is added or a single miter point. */
  const bool is_outside_corner = (sin_angle >= 0.0f);
  if (is_outside_corner && miter_limit_angle <= GP_STROKE_MITER_ANGLE_ROUND) {
    generate_arc_from_point_to_point(pt_b + normal_prev * radius,
                                     pt_b + normal * radius,
                                     pt_b,
                                     corner_subdivisions,
                                     src_point_index,
                                     r_perimeter,
                                     r_src_indices);
    return;
  }

  const float2 avg_tangent = math::normalize(tangent_prev + tangent);
  const float3 miter = {avg_tangent.y, -avg_tangent.x, 0.0f};
  const float miter_invscale = math::dot(normal, miter);

  if (is_outside_corner) {
    const bool is_bevel = -math::dot(tangent, tangent_prev) > math::cos(miter_limit_angle);
    if (is_bevel) {
      r_perimeter.append(pt_b + normal_prev * radius);
      r_perimeter.append(pt_b + normal * radius);
      r_src_indices.append_n_times(src_point_index, 2);
      return;
    }
    else {
      const float3 miter_point = pt_b + miter * radius / miter_invscale;

      r_perimeter.append(miter_point);
      r_src_indices.append(src_point_index);
      return;
    }
  }

  /* Avoid division by tiny values for steep angles. */
  const float3 miter_point = (radius < length * miter_invscale &&
                              radius < length_prev * miter_invscale) ?
                                 pt_b + miter * radius / miter_invscale :
                                 pt_b + miter * radius;

  r_perimeter.append(miter_point);
  r_src_indices.append(src_point_index);
}

static void generate_stroke_perimeter(const Span<float3> all_positions,
                                      const Span<float> all_radii,
                                      const IndexRange points,
                                      const int corner_subdivisions,
                                      const bool is_cyclic,
                                      const bool use_caps,
                                      const eGPDstroke_Caps start_cap_type,
                                      const eGPDstroke_Caps end_cap_type,
                                      const VArray<float> miter_angles,
                                      const float outline_offset,
                                      Vector<float3> &r_perimeter,
                                      Vector<int> &r_point_counts,
                                      Vector<int> &r_point_indices)
{
  const Span<float3> positions = all_positions.slice(points);
  const int point_num = points.size();
  if (point_num == 0) {
    return;
  }
  if (point_num == 1) {
    /* Generate a circle for a single point. */
    const int perimeter_start = r_perimeter.size();
    const int point = points.first();
    const float radius = std::max(all_radii[point] + outline_offset, 0.0f);
    generate_circle_from_point(
        positions.first(), radius, corner_subdivisions, point, r_perimeter, r_point_indices);
    const int perimeter_count = r_perimeter.size() - perimeter_start;
    if (perimeter_count > 0) {
      r_point_counts.append(perimeter_count);
    }
    return;
  }

  auto add_corner = [&](const int a, const int b, const int c) {
    const int point = points[b];
    const float3 pt_a = positions[a];
    const float3 pt_b = positions[b];
    const float3 pt_c = positions[c];
    const float radius = std::max(all_radii[point] + outline_offset, 0.0f);
    const float miter_angle = miter_angles[point];
    generate_corner(pt_a,
                    pt_b,
                    pt_c,
                    radius,
                    miter_angle,
                    corner_subdivisions,
                    point,
                    r_perimeter,
                    r_point_indices);
  };
  auto add_cap = [&](const int center_i, const int next_i, const eGPDstroke_Caps cap_type) {
    const int point = points[center_i];
    const float3 &center = positions[center_i];
    const float3 dir = math::normalize(positions[next_i] - center);
    const float radius = std::max(all_radii[point] + outline_offset, 0.0f);
    generate_cap(
        center, dir, radius, corner_subdivisions, cap_type, point, r_perimeter, r_point_indices);
  };

  /* Creates a single cyclic curve with end caps. */
  if (use_caps) {
    /* Open curves generate a start and end cap and a connecting stroke on either side. */
    const int perimeter_start = r_perimeter.size();

    /* Start cap. */
    add_cap(0, 1, start_cap_type);

    /* Right perimeter half. */
    for (const int i : points.index_range().drop_front(1).drop_back(1)) {
      add_corner(i - 1, i, i + 1);
    }
    if (is_cyclic) {
      add_corner(point_num - 2, point_num - 1, 0);
    }

    /* End cap. */
    if (is_cyclic) {
      /* End point is same as start point. */
      add_cap(0, point_num - 1, end_cap_type);
    }
    else {
      /* End point is last point of the curve. */
      add_cap(point_num - 1, point_num - 2, end_cap_type);
    }

    /* Left perimeter half. */
    if (is_cyclic) {
      add_corner(0, point_num - 1, point_num - 2);
    }
    for (const int i : points.index_range().drop_front(1).drop_back(1)) {
      add_corner(point_num - i, point_num - i - 1, point_num - i - 2);
    }

    const int perimeter_count = r_perimeter.size() - perimeter_start;
    if (perimeter_count > 0) {
      r_point_counts.append(perimeter_count);
    }
  }
  else {
    /* Generate separate "inside" and an "outside" perimeter curves.
     * The distinction is arbitrary, called left/right here. */

    /* Right side perimeter. */
    const int left_perimeter_start = r_perimeter.size();
    add_corner(point_num - 1, 0, 1);
    for (const int i : points.index_range().drop_front(1).drop_back(1)) {
      add_corner(i - 1, i, i + 1);
    }
    add_corner(point_num - 2, point_num - 1, 0);
    const int left_perimeter_count = r_perimeter.size() - left_perimeter_start;
    if (left_perimeter_count > 0) {
      r_point_counts.append(left_perimeter_count);
    }

    /* Left side perimeter. */
    const int right_perimeter_start = r_perimeter.size();
    add_corner(0, point_num - 1, point_num - 2);
    for (const int i : points.index_range().drop_front(1).drop_back(1)) {
      add_corner(point_num - i, point_num - i - 1, point_num - i - 2);
    }
    add_corner(1, 0, point_num - 1);
    const int right_perimeter_count = r_perimeter.size() - right_perimeter_start;
    if (right_perimeter_count > 0) {
      r_point_counts.append(right_perimeter_count);
    }
  }
}

struct PerimeterData {
  /* New points per curve count. */
  Vector<int> point_counts;
  /* New point coordinates. */
  Vector<float3> positions;
  /* Source curve index. */
  Vector<int> curve_indices;
  /* Source point index. */
  Vector<int> point_indices;
};

bke::CurvesGeometry create_curves_outline(const bke::greasepencil::Drawing &drawing,
                                          const IndexMask &strokes,
                                          const float4x4 &transform,
                                          const int corner_subdivisions,
                                          const float outline_radius,
                                          const float outline_offset,
                                          const int material_index)
{
  const bke::CurvesGeometry &src_curves = drawing.strokes();
  Span<float3> src_positions = src_curves.positions();
  bke::AttributeAccessor src_attributes = src_curves.attributes();
  const VArray<float> src_radii = drawing.radii();
  const VArray<bool> src_cyclic = *src_attributes.lookup_or_default(
      "cyclic", bke::AttrDomain::Curve, false);
  const VArray<int8_t> src_start_caps = *src_attributes.lookup_or_default<int8_t>(
      "start_cap", bke::AttrDomain::Curve, GP_STROKE_CAP_ROUND);
  const VArray<int8_t> src_end_caps = *src_attributes.lookup_or_default<int8_t>(
      "end_cap", bke::AttrDomain::Curve, GP_STROKE_CAP_ROUND);
  const VArray<int> src_material_index = *src_attributes.lookup_or_default(
      "material_index", bke::AttrDomain::Curve, 0);
  const VArray<float> miter_angles = *src_attributes.lookup_or_default<float>(
      "miter_angle", bke::AttrDomain::Point, GP_STROKE_MITER_ANGLE_ROUND);

  /* Transform positions and radii. */
  Array<float3> transformed_positions(src_positions.size());
  math::transform_points(src_positions, transform, transformed_positions);

  Array<float> transformed_radii(src_radii.size());
  const float scale = math::average(math::to_scale(transform));
  threading::parallel_for(transformed_radii.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      transformed_radii[i] = src_radii[i] * scale;
    }
  });

  const float4x4 transform_inv = math::invert(transform);
  threading::EnumerableThreadSpecific<PerimeterData> thread_data;
  strokes.foreach_index(GrainSize(256), [&](const int64_t curve_i) {
    PerimeterData &data = thread_data.local();

    const bool is_cyclic_curve = src_cyclic[curve_i];
    /* NOTE: Cyclic curves would better be represented by a cyclic perimeter without end caps, but
     * we always generate caps for compatibility with GPv2. Fill materials cannot create holes, so
     * a cyclic outline does not work well. */
    const bool use_caps = true /*!is_cyclic_curve*/;

    const int prev_point_num = data.positions.size();
    const int prev_curve_num = data.point_counts.size();
    const IndexRange points = src_curves.points_by_curve()[curve_i];

    generate_stroke_perimeter(transformed_positions,
                              transformed_radii,
                              points,
                              corner_subdivisions,
                              is_cyclic_curve,
                              use_caps,
                              eGPDstroke_Caps(src_start_caps[curve_i]),
                              eGPDstroke_Caps(src_end_caps[curve_i]),
                              miter_angles,
                              outline_offset,
                              data.positions,
                              data.point_counts,
                              data.point_indices);

    /* Transform perimeter positions back into object space. */
    math::transform_points(transform_inv,
                           data.positions.as_mutable_span().drop_front(prev_point_num));

    data.curve_indices.append_n_times(curve_i, data.point_counts.size() - prev_curve_num);
  });

  int dst_curve_num = 0;
  int dst_point_num = 0;
  for (const PerimeterData &data : thread_data) {
    BLI_assert(data.point_counts.size() == data.curve_indices.size());
    BLI_assert(data.positions.size() == data.point_indices.size());
    dst_curve_num += data.point_counts.size();
    dst_point_num += data.positions.size();
  }

  bke::CurvesGeometry dst_curves(dst_point_num, dst_curve_num);
  if (dst_point_num == 0 || dst_curve_num == 0) {
    return dst_curves;
  }

  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();
  bke::SpanAttributeWriter<bool> dst_cyclic = dst_attributes.lookup_or_add_for_write_span<bool>(
      "cyclic", bke::AttrDomain::Curve);
  bke::SpanAttributeWriter<int> dst_material = dst_attributes.lookup_or_add_for_write_span<int>(
      "material_index", bke::AttrDomain::Curve);
  bke::SpanAttributeWriter<float> dst_radius = dst_attributes.lookup_or_add_for_write_span<float>(
      "radius", bke::AttrDomain::Point);
  const MutableSpan<int> dst_offsets = dst_curves.offsets_for_write();
  const MutableSpan<float3> dst_positions = dst_curves.positions_for_write();
  /* Source indices for attribute mapping. */
  Array<int> dst_curve_map(dst_curve_num);
  Array<int> dst_point_map(dst_point_num);

  IndexRange curves;
  IndexRange points;
  for (const PerimeterData &data : thread_data) {
    curves = curves.after(data.point_counts.size());
    points = points.after(data.positions.size());

    /* Append curve data. */
    dst_curve_map.as_mutable_span().slice(curves).copy_from(data.curve_indices);
    /* Curve offsets are accumulated below. */
    dst_offsets.slice(curves).copy_from(data.point_counts);
    dst_cyclic.span.slice(curves).fill(true);
    if (material_index >= 0) {
      dst_material.span.slice(curves).fill(material_index);
    }
    else {
      for (const int i : curves.index_range()) {
        dst_material.span[curves[i]] = src_material_index[data.curve_indices[i]];
      }
    }

    /* Append point data. */
    dst_positions.slice(points).copy_from(data.positions);
    dst_point_map.as_mutable_span().slice(points).copy_from(data.point_indices);
    dst_radius.span.slice(points).fill(outline_radius);
  }
  offset_indices::accumulate_counts_to_offsets(dst_curves.offsets_for_write());

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Point,
                         bke::AttrDomain::Point,
                         bke::attribute_filter_from_skip_ref({"position", "radius"}),
                         dst_point_map,
                         dst_attributes);
  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic", "material_index"}),
                         dst_curve_map,
                         dst_attributes);

  dst_cyclic.finish();
  dst_material.finish();
  dst_radius.finish();
  dst_curves.update_curve_types();

  return dst_curves;
}

namespace trim {

enum Side : uint8_t { Start = 0, End = 1 };
enum Distance : uint8_t { Min = 0, Max = 1 };

/* When looking for intersections, we need a little padding, otherwise we could miss curves
 * that intersect for the eye, but not in hard numbers. */
static constexpr int BBOX_PADDING = 2;

/* When creating new intersection points, we don't want them too close to their neighbor,
 * because that clutters the geometry. This threshold defines what 'too close' is. */
static constexpr float DISTANCE_FACTOR_THRESHOLD = 0.01f;

/**
 * Structure describing a curve segment (a point range in a curve) with end intersection points.
 * A Segment can go past the end of the source curve and loop back to the start.
 */
class Segment {
 public:
  /* Curve index. */
  int curve = -1;

  /* The start and end of the original curve is stored, because this segment may go past then end
   * and have to loop. */
  IndexRange src_points;

  /* Point range of the segment: starting point and end point. Matches the point offsets
   * in a CurvesGeometry. */
  int points[2] = {-1, -1};

  /* The normalized distance where the trim segment is intersected by another curve.
   * For the outer ends of the trim segment the intersection distance is given between:
   * - [start point] and [start point + 1]
   * - [end point] and [end point + 1]
   */
  float alpha[2] = {0.0f, 0.0f};

  int intersection_index[2] = {-1, -1};

 public:
  constexpr Segment() = default;

  bool is_loop() const
  {
    /* If both intersection points are the same, then it's a full loop. */
    if (points[Side::Start] == points[Side::End]) {
      if (alpha[Side::Start] == alpha[Side::End]) {
        return true;
      }
    }

    return alpha[Side::End] == 1.0f;
  }

  bool has_intersection(const Side side) const
  {
    return alpha[side] != 0.0f && alpha[side] != 1.0f;
  }

  int2 edge(const Side side) const
  {
    return int2(points[side], this->wrap_index(points[side] + 1));
  }

  int wrap_index(const int i) const
  {
    return math::mod_periodic(i - src_points.first(), src_points.size()) + src_points.first();
  }

  IndexRange point_range() const
  {
    if (this->is_loop()) {
      return src_points;
    }

    if (!this->has_intersection(Side::Start) && this->has_intersection(Side::End)) {
      return IndexRange::from_begin_end_inclusive(src_points.first(), points[Side::End]);
    }

    if (!this->has_intersection(Side::Start) && !this->has_intersection(Side::End)) {
      return src_points;
    }

    /* If both intersection points are on the same edge, there's ether no points between or
     * all of the points are. */
    if (points[Side::Start] == points[Side::End]) {
      if (alpha[Side::Start] >= alpha[Side::End]) {
        return src_points.shift(points[Side::Start] - src_points.first() + 1);
      }
      return IndexRange(0);
    }

    if (points[Side::Start] > points[Side::End]) {
      return IndexRange::from_begin_end_inclusive(points[Side::Start] + 1,
                                                  points[Side::End] + src_points.size());
    }

    return IndexRange::from_begin_end_inclusive(points[Side::Start] + 1, points[Side::End]);
  }

  int points_num() const
  {
    return this->point_range().size();
  }

  template<typename Fn> inline void foreach_point(Fn &&fn) const
  {
    const IndexRange point_range = this->point_range();

    for (const int64_t pos : point_range.index_range()) {
      const int i = this->wrap_index(point_range[pos]);

      if constexpr (std::is_invocable_r_v<void, Fn, int64_t, int64_t>) {
        fn(i, pos);
      }
      else {
        fn(i);
      }
    }
  }

  constexpr static Segment from_curve(const int curve_i,
                                      const IndexRange points,
                                      const bool cyclical)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.src_points = points;

    segment.points[Side::Start] = points.first();
    segment.points[Side::End] = points.last();

    segment.alpha[Side::Start] = 0.0f;
    segment.alpha[Side::End] = cyclical ? 1.0f : 0.0f;

    return segment;
  }

  static Segment from_intersections(const int curve_i,
                                    const IndexRange points,
                                    const std::optional<float> parameter_start,
                                    const std::optional<float> parameter_end,
                                    const std::optional<int> inter_index_start,
                                    const std::optional<int> inter_index_end)
  {
    Segment segment;
    segment.curve = curve_i;
    segment.src_points = points;

    if (parameter_start) {
      segment.points[Side::Start] = int(math::floor(*parameter_start));
      segment.alpha[Side::Start] = math::fract(*parameter_start);
    }
    else {
      segment.points[Side::Start] = points.first();
      segment.alpha[Side::Start] = 0.0f;
    }

    if (inter_index_start) {
      segment.intersection_index[Side::Start] = *inter_index_start;
    }

    if (parameter_end) {
      segment.points[Side::End] = int(math::floor(*parameter_end));
      segment.alpha[Side::End] = math::fract(*parameter_end);
    }
    else {
      segment.points[Side::End] = points.last();
      segment.alpha[Side::End] = 0.0f;
    }

    if (inter_index_end) {
      segment.intersection_index[Side::End] = *inter_index_end;
    }

    return segment;
  }
};

/**
 * Get the intersection distance of two line segments a-b and c-d.
 * The intersection distance is defined as the normalized distance (0..1)
 * from point a to the intersection point of a-b and c-d.
 */
static float get_intersection_distance_of_segments(const float2 &co_a,
                                                   const float2 &co_b,
                                                   const float2 &co_c,
                                                   const float2 &co_d)
{
  /* Get intersection point. */
  const float a1 = co_b[1] - co_a[1];
  const float b1 = co_a[0] - co_b[0];
  const float c1 = a1 * co_a[0] + b1 * co_a[1];

  const float a2 = co_d[1] - co_c[1];
  const float b2 = co_c[0] - co_d[0];
  const float c2 = a2 * co_c[0] + b2 * co_c[1];

  const float det = (a1 * b2 - a2 * b1);
  if (det == 0.0f) {
    return 0.0f;
  }

  float2 isect((b2 * c1 - b1 * c2) / det, (a1 * c2 - a2 * c1) / det);

  /* Get normalized distance from point a to intersection point. */
  const float length_ab = math::length(co_b - co_a);
  const float distance = (length_ab == 0.0f ?
                              0.0f :
                              math::clamp(math::length(isect - co_a) / length_ab, 0.0f, 1.0f));

  /* Snap to the ends if very close. */
  if (math::abs(distance) < 0.0001f) {
    return 0.0f;
  }
  if (math::abs(distance - 1.0f) < 0.0001f) {
    return 1.0f;
  }

  return distance;
}

static void calculate_offsets_from_segments(const Span<Segment> segments,
                                            const OffsetIndices<int> segment_offsets,
                                            const Span<bool> cyclic,
                                            MutableSpan<int> offsets)
{
  int offset = 0;

  for (const int curve_i : segment_offsets.index_range()) {
    offsets[curve_i] = offset;

    const IndexRange segment_range = segment_offsets[curve_i];
    for (const int seg_i : segment_range) {
      const Segment &segment = segments[seg_i];

      if (segment.has_intersection(Side::Start) && !segment.is_loop()) {
        offset++;
      }
      offset += segment.points_num();
      if (seg_i == segment_range.last() && segment.has_intersection(Side::End) && !cyclic[curve_i])
      {
        offset++;
      }
    }
  }

  offsets.last() = offset;
}

static bke::CurvesGeometry create_curves_from_segments(const bke::CurvesGeometry &src,
                                                       const Span<Segment> segments,
                                                       const Span<bool> segment_reversed,
                                                       const Span<bool> cyclic,
                                                       const OffsetIndices<int> segment_offsets)
{
  Array<int> point_offsets(segment_offsets.size() + 1);
  calculate_offsets_from_segments(
      segments, segment_offsets, cyclic, point_offsets.as_mutable_span());

  const bke::AttributeAccessor src_attributes = src.attributes();

  const OffsetIndices<int> dst_points_by_curve = OffsetIndices<int>(point_offsets);

  if (dst_points_by_curve.total_size() == 0) {
    return bke::CurvesGeometry();
  }

  bke::CurvesGeometry dst_curves(dst_points_by_curve.total_size(), dst_points_by_curve.size());
  bke::MutableAttributeAccessor dst_attributes = dst_curves.attributes_for_write();

  dst_curves.offsets_for_write().copy_from(dst_points_by_curve.data());
  dst_curves.cyclic_for_write().copy_from(cyclic);

  Array<int> old_by_new_map(dst_points_by_curve.size());

  for (const int i : dst_points_by_curve.index_range()) {
    const IndexRange segment_range = segment_offsets[i];
    old_by_new_map[i] = segments[segment_range.first()].curve;
  }

  bke::gather_attributes(src_attributes,
                         bke::AttrDomain::Curve,
                         bke::AttrDomain::Curve,
                         bke::attribute_filter_from_skip_ref({"cyclic"}),
                         old_by_new_map,
                         dst_attributes);

  /* Copy/Interpolate point attributes. */
  for (auto &attribute : bke::retrieve_attributes_for_transfer(
           src_attributes, dst_attributes, ATTR_DOMAIN_MASK_POINT, {}))
  {
    bke::attribute_math::convert_to_static_type(attribute.dst.span.type(), [&](auto dummy) {
      using T = decltype(dummy);
      auto src_attr = attribute.src.typed<T>();
      auto dst_attr = attribute.dst.span.typed<T>();

      int i = 0;

      for (const int curve_i : segment_offsets.index_range()) {
        const IndexRange segment_range = segment_offsets[curve_i];
        for (const int seg_i : segment_range) {
          const Segment &segment = segments[seg_i];
          const bool reversed = segment_reversed[seg_i];

          if (reversed ? segment.has_intersection(Side::End) :
                         segment.has_intersection(Side::Start) && !segment.is_loop())
          {
            const float start_alpha = reversed ? segment.alpha[Side::End] :
                                                 segment.alpha[Side::Start];
            const int2 start_edge = reversed ? segment.edge(Side::End) : segment.edge(Side::Start);
            dst_attr[i++] = bke::attribute_math::mix2<T>(
                start_alpha, src_attr[start_edge.x], src_attr[start_edge.y]);
          }

          segment.foreach_point(
              [&](const int index, const int pos) { dst_attr[pos + i] = src_attr[index]; });

          if (reversed) {
            dst_attr.slice(IndexRange::from_begin_size(i, segment.points_num())).reverse();
          }

          i += segment.points_num();

          if (seg_i == segment_range.last() &&
              (reversed ? segment.has_intersection(Side::Start) :
                          segment.has_intersection(Side::End)) &&
              !cyclic[curve_i])
          {
            const float end_alpha = reversed ? segment.alpha[Side::Start] :
                                               segment.alpha[Side::End];
            const int2 end_edge = reversed ? segment.edge(Side::Start) : segment.edge(Side::End);
            dst_attr[i++] = bke::attribute_math::mix2<T>(
                end_alpha, src_attr[end_edge.x], src_attr[end_edge.y]);
          }
        }
      }
    });

    attribute.dst.finish();
  }

  return dst_curves;
}

struct IntersectionPoint {
  int point_i = -1;
  int point_j = -1;
  float alpha_i = -1.0f;
  float alpha_j = -1.0f;
  int curve_i = -1;
  int curve_j = -1;

  int segment_index_i[2] = {-1, -1};
  int segment_index_j[2] = {-1, -1};

  constexpr IntersectionPoint() = default;

  float parameter_for_curve(const int curve) const
  {
    BLI_assert(curve == curve_i || curve == curve_j);
    return curve == curve_i ? point_i + alpha_i : point_j + alpha_j;
  }
};

static IntersectionPoint create_intersection(const int point_i,
                                             const int point_j,
                                             const float alpha_i,
                                             const float alpha_j,
                                             const int curve_i,
                                             const int curve_j)
{
  IntersectionPoint inter_point;
  inter_point.point_i = point_i;
  inter_point.point_j = point_j;
  inter_point.alpha_i = alpha_i;
  inter_point.alpha_j = alpha_j;
  inter_point.curve_i = curve_i;
  inter_point.curve_j = curve_j;

  return inter_point;
}

static void find_intersections_between_curve_and_curves(const int curve_i,
                                                        const Span<float2> screen_space_positions,
                                                        const Span<rcti> screen_space_curve_bounds,
                                                        const OffsetIndices<int> points_by_curve,
                                                        const VArray<bool> &cyclic,
                                                        const IndexMask &visible_curves,
                                                        Array<Vector<int>> &r_inters_per_curves,
                                                        Vector<IntersectionPoint> &r_intersections)
{
  const bool cyclic_i = cyclic[curve_i];
  const IndexRange points_i = points_by_curve[curve_i];

  for (const int i : points_i.index_range().drop_back(cyclic_i ? 0 : 1)) {
    const int point_i1 = points_i[i];
    const int point_i2 = points_i[(i + 1) % points_i.size()];

    const float2 co_i1 = screen_space_positions[point_i1];
    const float2 co_i2 = screen_space_positions[point_i2];

    rcti bbox_i;
    BLI_rcti_init_minmax(&bbox_i);
    BLI_rcti_do_minmax_v(&bbox_i, int2(co_i1));
    BLI_rcti_do_minmax_v(&bbox_i, int2(co_i2));
    BLI_rcti_pad(&bbox_i, BBOX_PADDING, BBOX_PADDING);

    /* Add some padding to the line segment i1-i2, otherwise we could just miss an
     * intersection. */
    const float2 padding_i = math::normalize(co_i2 - co_i1);
    const float2 padded_i1 = co_i1 - padding_i;
    const float2 padded_i2 = co_i2 + padding_i;

    visible_curves.foreach_index([&](const int curve_j) {
      if (curve_i > curve_j) {
        return;
      }

      /* Bounding box check: skip curves that don't overlap segment i1-i2. */
      if (!BLI_rcti_isect(&bbox_i, &screen_space_curve_bounds[curve_j], nullptr)) {
        return;
      }

      const bool cyclic_j = cyclic[curve_j];
      const IndexRange points_j = points_by_curve[curve_j];

      for (const int j : points_j.index_range().drop_back(cyclic_j ? 0 : 1)) {
        const int point_j1 = points_j[j];
        const int point_j2 = points_j[(j + 1) % points_j.size()];

        /* Don't self check. */
        if (curve_i == curve_j && (point_i1 == point_j1 || point_i1 == point_j2 ||
                                   point_i2 == point_j1 || point_i2 == point_j2))
        {
          continue;
        }

        const float2 co_j1 = screen_space_positions[point_j1];
        const float2 co_j2 = screen_space_positions[point_j2];

        /* Skip when bounding boxes of i1-i2 and j1-j2 don't overlap. */
        rcti bbox_j;
        BLI_rcti_init_minmax(&bbox_j);
        BLI_rcti_do_minmax_v(&bbox_j, int2(co_j1));
        BLI_rcti_do_minmax_v(&bbox_j, int2(co_j2));
        BLI_rcti_pad(&bbox_j, BBOX_PADDING, BBOX_PADDING);
        if (!BLI_rcti_isect(&bbox_i, &bbox_j, nullptr)) {
          continue;
        }

        /* Add some padding to the line segment j1-j2, otherwise we could just miss an
         * intersection. */
        const float2 padding_j = math::normalize(co_j2 - co_j1);
        const float2 padded_j1 = co_j1 - padding_j;
        const float2 padded_j2 = co_j2 + padding_j;

        /* Check for intersection. */
        const auto isect = math::isect_seg_seg(padded_i1, padded_i2, padded_j1, padded_j2);
        if (ELEM(isect.kind, isect.LINE_LINE_CROSS, isect.LINE_LINE_EXACT)) {
          const float alpha_i = get_intersection_distance_of_segments(co_i1, co_i2, co_j1, co_j2);
          const float alpha_j = get_intersection_distance_of_segments(co_j1, co_j2, co_i1, co_i2);

          r_inters_per_curves[curve_i].append(r_intersections.size());
          r_inters_per_curves[curve_j].append(r_intersections.size());
          r_intersections.append(
              create_intersection(point_i1, point_j1, alpha_i, alpha_j, curve_i, curve_j));
        }
      }
    });
  }
}

static void find_intersections_between_all_curves(const Span<float2> screen_space_positions,
                                                  const Span<rcti> screen_space_curve_bounds,
                                                  const OffsetIndices<int> points_by_curve,
                                                  const VArray<bool> &cyclic,
                                                  const IndexMask &visible_curves,
                                                  Array<Vector<int>> &r_inters_per_curves,
                                                  Vector<IntersectionPoint> &r_intersections)
{
  visible_curves.foreach_index([&](const int curve_i) {
    find_intersections_between_curve_and_curves(curve_i,
                                                screen_space_positions,
                                                screen_space_curve_bounds,
                                                points_by_curve,
                                                cyclic,
                                                visible_curves,
                                                r_inters_per_curves,
                                                r_intersections);
  });
}

static void add_segments(const int curve_k,
                         const Span<Vector<int>> inters_per_curves,
                         const OffsetIndices<int> points_by_curve,
                         const Span<IntersectionPoint> &intersections,
                         const VArray<bool> &cyclic,
                         Vector<Segment> &all_segments,
                         MutableSpan<IndexRange> all_segments_by_curve)
{
  const IndexRange points_k = points_by_curve[curve_k];
  const Span<int> inters = inters_per_curves[curve_k];

  const int start_size = all_segments.size();

  if (inters.size() == 0) {
    all_segments.append(Segment::from_curve(curve_k, points_k, cyclic[curve_k]));
    all_segments_by_curve[curve_k] = all_segments.index_range().drop_front(start_size);

    return;
  }

  Array<int> inter_sorted_ids = Array<int>(inters.size());
  array_utils::fill_index_range<int>(inter_sorted_ids);

  parallel_sort(inter_sorted_ids.begin(), inter_sorted_ids.end(), [&](int i1, int i2) {
    const IntersectionPoint &inter1 = intersections[inters[i1]];
    const IntersectionPoint &inter2 = intersections[inters[i2]];
    return inter1.parameter_for_curve(curve_k) < inter2.parameter_for_curve(curve_k);
  });

  if (cyclic[curve_k]) {
    const int int_p_1 = inters[inter_sorted_ids.first()];
    const int int_p_2 = inters[inter_sorted_ids.last()];

    const IntersectionPoint &inter_first = intersections[int_p_1];
    const IntersectionPoint &inter_last = intersections[int_p_2];

    all_segments.append(Segment::from_intersections(curve_k,
                                                    points_k,
                                                    inter_last.parameter_for_curve(curve_k),
                                                    inter_first.parameter_for_curve(curve_k),
                                                    int_p_2,
                                                    int_p_1));
  }
  else {
    const int int_p_1 = inters[inter_sorted_ids.first()];
    const IntersectionPoint &inter_first = intersections[int_p_1];

    all_segments.append(Segment::from_intersections(curve_k,
                                                    points_k,
                                                    std::nullopt,
                                                    inter_first.parameter_for_curve(curve_k),
                                                    std::nullopt,
                                                    int_p_1));
  }

  for (const int inter_id : inter_sorted_ids.index_range().drop_back(1)) {
    const int int_p_1 = inters[inter_sorted_ids[inter_id]];
    const int int_p_2 = inters[inter_sorted_ids[inter_id + 1]];

    const IntersectionPoint &inter_first = intersections[int_p_1];
    const IntersectionPoint &inter_last = intersections[int_p_2];

    if (inter_first.parameter_for_curve(curve_k) != inter_last.parameter_for_curve(curve_k)) {
      all_segments.append(Segment::from_intersections(curve_k,
                                                      points_k,
                                                      inter_first.parameter_for_curve(curve_k),
                                                      inter_last.parameter_for_curve(curve_k),
                                                      int_p_1,
                                                      int_p_2));
    }
  }

  if (!(cyclic[curve_k])) {
    const int int_p_2 = inters[inter_sorted_ids.last()];
    const IntersectionPoint &inter_last = intersections[int_p_2];

    all_segments.append(Segment::from_intersections(curve_k,
                                                    points_k,
                                                    inter_last.parameter_for_curve(curve_k),
                                                    std::nullopt,
                                                    int_p_2,
                                                    std::nullopt));
  }

  all_segments_by_curve[curve_k] = all_segments.index_range().drop_front(start_size);
}

static bool check_and_join_segments(Segment &first, const Segment &second)
{
  if (first.curve != second.curve) {
    return false;
  }

  if (first.intersection_index[Side::End] == second.intersection_index[Side::Start] &&
      first.intersection_index[Side::End] != -1)
  {
    first.points[Side::End] = second.points[Side::End];
    first.alpha[Side::End] = second.alpha[Side::End];
    first.intersection_index[Side::End] = second.intersection_index[Side::End];

    return true;
  }
  if (first.intersection_index[Side::Start] == second.intersection_index[Side::End] &&
      first.intersection_index[Side::Start] != -1)
  {
    first.points[Side::Start] = second.points[Side::Start];
    first.alpha[Side::Start] = second.alpha[Side::Start];
    first.intersection_index[Side::Start] = second.intersection_index[Side::Start];

    return true;
  }

  return false;
}

static void calculate_cyclical_curves(const Span<Segment> segments,
                                      const OffsetIndices<int> segment_offsets,
                                      const Span<bool> segment_reversed,
                                      MutableSpan<bool> cyclic)
{
  for (const int curve_i : segment_offsets.index_range()) {
    const IndexRange segment_range = segment_offsets[curve_i];

    if (segment_range.size() == 1) {
      cyclic[curve_i] = segments[segment_range.first()].is_loop();
      continue;
    }

    const int segment_index_first = segment_range.first();
    const bool reversed_first = segment_reversed[segment_index_first];
    const Segment &segment_first = segments[segment_index_first];
    const Side direction_first = reversed_first ? Side::End : Side::Start;
    const int inter_index_first = segment_first.intersection_index[direction_first];

    const int segment_index_last = segment_range.last();
    const bool reversed_last = segment_reversed[segment_index_last];
    const Segment &segment_last = segments[segment_index_last];
    const Side direction_last = reversed_last ? Side::Start : Side::End;
    const int inter_index_last = segment_last.intersection_index[direction_last];

    /* Check if there is no intersection and therefor the segment has ends. */
    if (inter_index_first == -1 || inter_index_last == -1) {
      cyclic[curve_i] = false;
      continue;
    }

    /* Check if the segments end were they start. */
    cyclic[curve_i] = inter_index_first == inter_index_last;
  }
}

static void calculate_segment_directions(const Span<Segment> segments,
                                         const OffsetIndices<int> segment_offsets,
                                         MutableSpan<bool> segment_reversed)
{
  for (const int curve_i : segment_offsets.index_range()) {
    const IndexRange segment_range = segment_offsets[curve_i];

    segment_reversed[segment_range.first()] = false;
    for (const int segment_i : segment_range.drop_front(1)) {
      const bool reversed_prev = segment_reversed[segment_i - 1];
      const Segment &segment_prev = segments[segment_i - 1];
      const Segment &segment = segments[segment_i];
      const Side direction_prev = reversed_prev ? Side::Start : Side::End;
      const int inter_index_prev = segment_prev.intersection_index[direction_prev];

      if (segment.intersection_index[Side::Start] == inter_index_prev) {
        segment_reversed[segment_i] = false;
      }
      else {
        BLI_assert(segment.intersection_index[Side::End] == inter_index_prev);
        segment_reversed[segment_i] = true;
      }
    }
  }
}

static void cut_caps(bke::CurvesGeometry &dst,
                     const Span<Segment> segments,
                     const Span<bool> segment_reversed,
                     const Span<bool> cyclic,
                     const OffsetIndices<int> segment_offsets)
{
  bke::MutableAttributeAccessor dst_attributes = dst.attributes_for_write();

  bke::SpanAttributeWriter<int8_t> dst_start_caps =
      dst_attributes.lookup_or_add_for_write_span<int8_t>("start_cap", bke::AttrDomain::Curve);
  bke::SpanAttributeWriter<int8_t> dst_end_caps =
      dst_attributes.lookup_or_add_for_write_span<int8_t>("end_cap", bke::AttrDomain::Curve);

  for (const int curve_i : segment_offsets.index_range()) {
    /* If the curve connects back to it's self, don't cut it. */
    if (cyclic[curve_i]) {
      continue;
    }

    const IndexRange segment_range = segment_offsets[curve_i];

    const int segment_index_first = segment_range.first();
    const bool reversed_first = segment_reversed[segment_index_first];
    const Segment &segment_first = segments[segment_index_first];
    const Side direction_first = reversed_first ? Side::End : Side::Start;
    const int inter_index_first = segment_first.intersection_index[direction_first];

    const int segment_index_last = segment_range.last();
    const bool reversed_last = segment_reversed[segment_index_last];
    const Segment &segment_last = segments[segment_index_last];
    const Side direction_last = reversed_last ? Side::Start : Side::End;
    const int inter_index_last = segment_last.intersection_index[direction_last];

    /* Check if there is intersection and therefor the segment should be cut. */
    if (inter_index_first != -1) {
      dst_start_caps.span[curve_i] = GP_STROKE_CAP_TYPE_FLAT;
    }
    if (inter_index_last != -1) {
      dst_end_caps.span[curve_i] = GP_STROKE_CAP_TYPE_FLAT;
    }
  }

  dst_start_caps.finish();
  dst_end_caps.finish();
}

static constexpr int SEGMENT_CONNECTION_NULL = 0;

/* We store the side as sign, but because a segment with index zero is valid, we shift by one. */
static int encode_index_and_side(const int index, const Side side)
{
  return side == Side::Start ? index + 1 : -(index + 1);
}

static int decode_index(const int encoded)
{
  return math::abs(encoded) - 1;
}

static Side decode_side(const int encoded)
{
  return encoded < 0 ? Side::End : Side::Start;
}

static void create_connections_from_curves(const Span<IndexRange> segments_by_curve,
                                           const Span<bool> segments_to_keep,
                                           const VArray<bool> is_cyclic,
                                           MutableSpan<int2> segment_connections)
{
  for (const int curve_i : segments_by_curve.index_range()) {
    const IndexRange segment_range = segments_by_curve[curve_i];

    if (segment_range.size() == 1) {
      if (segments_to_keep[segment_range.first()]) {
        segment_connections[segment_range.first()][Side::Start] = SEGMENT_CONNECTION_NULL;
        segment_connections[segment_range.first()][Side::End] = SEGMENT_CONNECTION_NULL;
      }
      continue;
    }

    for (const int segment_i : segment_range.drop_back(1)) {
      if (!segments_to_keep[segment_i]) {
        continue;
      }

      if (segments_to_keep[segment_i + 1]) {
        segment_connections[segment_i][Side::End] = encode_index_and_side(segment_i + 1,
                                                                          Side::Start);
        segment_connections[segment_i + 1][Side::Start] = encode_index_and_side(segment_i,
                                                                                Side::End);
      }
      else {
        segment_connections[segment_i][Side::End] = SEGMENT_CONNECTION_NULL;
      }
    }

    if (!segments_to_keep[segment_range.last()]) {
      continue;
    }

    if (!is_cyclic[curve_i]) {
      segment_connections[segment_range.first()][Side::Start] = SEGMENT_CONNECTION_NULL;
      segment_connections[segment_range.last()][Side::End] = SEGMENT_CONNECTION_NULL;
      continue;
    }

    if (segments_to_keep[segment_range.first()]) {
      segment_connections[segment_range.first()][Side::Start] = encode_index_and_side(
          segment_range.last(), Side::End);
      segment_connections[segment_range.last()][Side::End] = encode_index_and_side(
          segment_range.first(), Side::Start);
    }
    else {
      segment_connections[segment_range.last()][Side::End] = SEGMENT_CONNECTION_NULL;
    }
  }
}

static void follow_segment_connections(const Span<Segment> all_segments,
                                       const Span<bool> segments_to_keep,
                                       const Span<int2> segment_connections,
                                       Vector<Segment> &segments,
                                       Vector<int> &segment_offset_data)
{
  BLI_assert(all_segments.size() == segments_to_keep.size());
  BLI_assert(all_segments.size() == segment_connections.size());

  segment_offset_data.append(0);

  /* Follow each segment until it loops or ends. */
  Array<bool> processed_segments(all_segments.size(), false);
  int start_segment = 0;

  auto get_next_unprocessed_segment = [&]() {
    /* All segment before `start_segment` are guaranteed to be processed, so skip search them.
     * This optimization make the algorithm `O(N)` instead of `O(N^2)`.*/
    const int empty_num = start_segment;
    const int first_segment = processed_segments.as_span().drop_front(empty_num).first_index_try(
        false);

    if (first_segment == -1) {
      return -1;
    }
    return first_segment + empty_num;
  };

  /* Mark all segments that are not to keep as processed. */
  for (const int seg_i : all_segments.index_range()) {
    if (!segments_to_keep[seg_i]) {
      processed_segments[seg_i] = true;
    }
  }

  /* Get the first unprocessed segment. */
  start_segment = get_next_unprocessed_segment();

  while (start_segment != -1) {
    bool current_backwards = false;
    int current_i = start_segment;

    bool curve_done = false;
    while (!curve_done) {
      if (processed_segments[current_i] == true) {
        BLI_assert_unreachable();
        break;
      }

      const Segment &current_segment = all_segments[current_i];
      processed_segments[current_i] = true;

      if (segments.size() == 0) {
        segments.append(current_segment);
      }
      /* Check if the last segment can be joined with this one. */
      else if (!check_and_join_segments(segments.last(), current_segment)) {
        segments.append(current_segment);
      }

      const int next_encoded =
          segment_connections[current_i][current_backwards ? Side::Start : Side::End];

      const int next_segment = decode_index(next_encoded);
      const Side next_side = decode_side(next_encoded);

      if (next_encoded == SEGMENT_CONNECTION_NULL) {
        curve_done = true;
        break;
      }

      if (next_segment == start_segment) {
        curve_done = true;

        BLI_assert(next_side == Side::Start);

        /* Check if the last segment in this curve can be joined to the first one in this curve. */
        if ((!segments.index_range().is_empty()) &&
            segment_offset_data.last() != segments.index_range().last())
        {
          if (check_and_join_segments(segments[segment_offset_data.last()], segments.last())) {
            segments.remove_last();
          }
        }

        break;
      }

      current_i = next_segment;
      current_backwards = next_side == Side::End;
    }
    segment_offset_data.append(segments.size());

    /* Get the next unprocessed segment. */
    start_segment = get_next_unprocessed_segment();
  }
}

static bool check_line_segment_lasso_intersection(const int2 pos_a,
                                                  const int2 pos_b,
                                                  const Span<int2> mcoords)
{
  rcti bbox_ab;
  BLI_rcti_init_minmax(&bbox_ab);
  BLI_rcti_do_minmax_v(&bbox_ab, pos_a);
  BLI_rcti_do_minmax_v(&bbox_ab, pos_b);
  BLI_rcti_pad(&bbox_ab, BBOX_PADDING, BBOX_PADDING);

  /* Check the lasso bounding box first as an optimization. */
  if (BLI_rcti_isect_segment(&bbox_ab, pos_a, pos_b) &&
      BLI_lasso_is_edge_inside(mcoords, pos_a.x, pos_a.y, pos_b.x, pos_b.y, IS_CLIPPED))
  {
    return true;
  }
  return false;
}

static void check_segments_in_lasso(const Span<float2> screen_space_positions,
                                    const Span<rcti> screen_space_curve_bounds,
                                    const Span<int2> mcoords,
                                    const Span<Segment> all_segments,
                                    const IndexMask &curve_selection,
                                    const Span<IndexRange> segments_by_curve,
                                    MutableSpan<bool> segments_to_keep)
{
  rcti bbox_lasso;
  BLI_lasso_boundbox(&bbox_lasso, mcoords);

  curve_selection.foreach_index([&](const int curve_i) {
    /* To speed things up: do a bounding box check on the curve and the lasso area. */
    if (!BLI_rcti_isect(&bbox_lasso, &screen_space_curve_bounds[curve_i], nullptr)) {
      return;
    }

    const IndexRange &segment_range = segments_by_curve[curve_i];
    for (const int segment_i : segment_range) {
      const Segment &segment = all_segments[segment_i];

      const IndexRange point_range = segment.point_range();

      if (point_range.is_empty()) {
        const float start_alpha = segment.alpha[Side::Start];
        const int2 start_edge = segment.edge(Side::Start);
        const float end_alpha = segment.alpha[Side::End];
        const int2 end_edge = segment.edge(Side::End);
        const float2 pos_1 = math::interpolate(screen_space_positions[start_edge.x],
                                               screen_space_positions[start_edge.y],
                                               start_alpha);
        const float2 pos_2 = math::interpolate(
            screen_space_positions[end_edge.x], screen_space_positions[end_edge.y], end_alpha);

        if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
          segments_to_keep[segment_i] = false;
        }

        continue;
      }

      for (const int64_t i : point_range.drop_back(1)) {
        const int point_i1 = segment.wrap_index(i);
        const int point_i2 = segment.wrap_index(i + 1);

        const float2 pos_1 = screen_space_positions[point_i1];
        const float2 pos_2 = screen_space_positions[point_i2];

        if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
          segments_to_keep[segment_i] = false;
          continue;
        }
      }

      if (segment_range.size() == 1 && segment.is_loop()) {
        const float2 pos_1 = screen_space_positions[segment.wrap_index(point_range.first())];
        const float2 pos_2 = screen_space_positions[segment.wrap_index(point_range.last())];

        if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
          segments_to_keep[segment_i] = false;
          continue;
        }
      }
      else {
        if (segment.has_intersection(Side::Start)) {
          const float start_alpha = segment.alpha[Side::Start];
          const int2 start_edge = segment.edge(Side::Start);
          const float2 pos_1 = math::interpolate(screen_space_positions[start_edge.x],
                                                 screen_space_positions[start_edge.y],
                                                 start_alpha);
          const float2 pos_2 = screen_space_positions[segment.wrap_index(point_range.first())];

          if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
            segments_to_keep[segment_i] = false;
            continue;
          }
        }

        if (segment.has_intersection(Side::End)) {
          const float end_alpha = segment.alpha[Side::End];
          const int2 end_edge = segment.edge(Side::End);
          const float2 pos_1 = screen_space_positions[segment.wrap_index(point_range.last())];
          const float2 pos_2 = math::interpolate(
              screen_space_positions[end_edge.x], screen_space_positions[end_edge.y], end_alpha);

          if (check_line_segment_lasso_intersection(int2(pos_1), int2(pos_2), mcoords)) {
            segments_to_keep[segment_i] = false;
            continue;
          }
        }
      }
    }
  });
}

static void store_segment_map_on_intersections(const Span<Segment> all_segments,
                                               MutableSpan<IntersectionPoint> intersections)
{
  for (const int seg_i : all_segments.index_range()) {
    const Segment &segment = all_segments[seg_i];
    const int curve_i = segment.curve;

    if (segment.has_intersection(Side::Start)) {
      IntersectionPoint &inter_start = intersections[segment.intersection_index[Side::Start]];
      if (curve_i == inter_start.curve_i) {
        inter_start.segment_index_i[Side::End] = seg_i;
      }
      else {
        inter_start.segment_index_j[Side::End] = seg_i;
      }
    }

    if (segment.has_intersection(Side::End)) {
      IntersectionPoint &inter_end = intersections[segment.intersection_index[Side::End]];
      if (curve_i == inter_end.curve_i) {
        inter_end.segment_index_i[Side::Start] = seg_i;
      }
      else {
        inter_end.segment_index_j[Side::Start] = seg_i;
      }
    }
  }
}

bke::CurvesGeometry trim_curve_segments(const bke::CurvesGeometry &src,
                                        const Span<float2> screen_space_positions,
                                        const Span<rcti> screen_space_curve_bounds,
                                        const Span<int2> mcoords,
                                        const IndexMask &curve_selection,
                                        const IndexMask &visible_curves,
                                        const bool keep_caps)
{
  const OffsetIndices<int> src_points_by_curve = src.points_by_curve();
  const VArray<bool> is_cyclic = src.cyclic();

  if (src.is_empty()) {
    return src;
  }

  Vector<IntersectionPoint> intersections;
  Array<IndexRange> segments_by_curve(src_points_by_curve.size());
  Vector<Segment> all_segments;

  /* -------------------- */

  Array<Vector<int>> inters_per_curves(src_points_by_curve.size());
  find_intersections_between_all_curves(screen_space_positions,
                                        screen_space_curve_bounds,
                                        src_points_by_curve,
                                        is_cyclic,
                                        visible_curves,
                                        inters_per_curves,
                                        intersections);

  for (const int curve_i : src_points_by_curve.index_range()) {
    add_segments(curve_i,
                 inters_per_curves,
                 src_points_by_curve,
                 intersections,
                 is_cyclic,
                 all_segments,
                 segments_by_curve);
  }

  /* -------------------- */

  Array<bool> segments_to_keep(all_segments.size(), true);
  check_segments_in_lasso(screen_space_positions,
                          screen_space_curve_bounds,
                          mcoords,
                          all_segments,
                          curve_selection,
                          segments_by_curve,
                          segments_to_keep.as_mutable_span());

  /* -------------------- */

  Array<int2> segment_connections(all_segments.size(), int2(SEGMENT_CONNECTION_NULL));
  create_connections_from_curves(
      segments_by_curve, segments_to_keep, is_cyclic, segment_connections.as_mutable_span());
  store_segment_map_on_intersections(all_segments, intersections);

  /* -------------------- */

  Vector<Segment> segments;
  Vector<int> segment_offset_data;
  follow_segment_connections(
      all_segments, segments_to_keep, segment_connections, segments, segment_offset_data);
  Array<bool> segment_reversed(segments.size());
  const OffsetIndices<int> segment_offsets = OffsetIndices<int>(segment_offset_data);
  Array<bool> cyclic(segment_offsets.size());

  /* -------------------- */

  calculate_segment_directions(segments, segment_offsets, segment_reversed.as_mutable_span());
  calculate_cyclical_curves(segments, segment_offsets, segment_reversed, cyclic.as_mutable_span());

  bke::CurvesGeometry dst = create_curves_from_segments(
      src, segments, segment_reversed, cyclic, segment_offsets);

  /* -------------------- */

  if (!keep_caps) {
    cut_caps(dst, segments, segment_reversed, cyclic, segment_offsets);
  }

  return dst;
}

bke::CurvesGeometry trim_curve_segment_ends(const bke::CurvesGeometry &src,
                                            const Span<float2> screen_space_positions,
                                            const Span<rcti> screen_space_curve_bounds,
                                            const IndexMask &curve_selection,
                                            const IndexMask &visible_curves,
                                            const bool keep_caps)
{
  const OffsetIndices<int> src_points_by_curve = src.points_by_curve();
  const VArray<bool> is_cyclic = src.cyclic();

  Vector<IntersectionPoint> intersections;
  Array<IndexRange> segments_by_curve(src_points_by_curve.size());
  Vector<Segment> all_segments;

  /* -------------------- */

  Array<Vector<int>> inters_per_curves(src_points_by_curve.size());
  find_intersections_between_all_curves(screen_space_positions,
                                        screen_space_curve_bounds,
                                        src_points_by_curve,
                                        is_cyclic,
                                        visible_curves,
                                        inters_per_curves,
                                        intersections);

  for (const int curve_i : src_points_by_curve.index_range()) {
    add_segments(curve_i,
                 inters_per_curves,
                 src_points_by_curve,
                 intersections,
                 is_cyclic,
                 all_segments,
                 segments_by_curve);
  }

  /* -------------------- */

  Array<bool> segments_to_keep(all_segments.size(), true);
  curve_selection.foreach_index(GrainSize(128), [&](const int curve_i) {
    const IndexRange segment_range = segments_by_curve[curve_i];

    if (segment_range.size() > 2) {
      segments_to_keep[segment_range.first()] = false;
      segments_to_keep[segment_range.last()] = false;
    }
  });

  /* -------------------- */

  Array<int2> segment_connections(all_segments.size(), int2(SEGMENT_CONNECTION_NULL));
  create_connections_from_curves(
      segments_by_curve, segments_to_keep, is_cyclic, segment_connections.as_mutable_span());
  store_segment_map_on_intersections(all_segments, intersections);

  /* -------------------- */

  Vector<Segment> segments;
  Vector<int> segment_offset_data;
  follow_segment_connections(
      all_segments, segments_to_keep, segment_connections, segments, segment_offset_data);
  Array<bool> segment_reversed(segments.size());
  const OffsetIndices<int> segment_offsets = OffsetIndices<int>(segment_offset_data);
  Array<bool> cyclic(segment_offsets.size());

  /* -------------------- */

  calculate_segment_directions(segments, segment_offsets, segment_reversed.as_mutable_span());
  calculate_cyclical_curves(segments, segment_offsets, segment_reversed, cyclic.as_mutable_span());

  bke::CurvesGeometry dst = create_curves_from_segments(
      src, segments, segment_reversed, cyclic, segment_offsets);

  /* -------------------- */

  if (!keep_caps) {
    cut_caps(dst, segments, segment_reversed, cyclic, segment_offsets);
  }

  return dst;
}

}  // namespace trim

Curves2DBVHTree build_curves_2d_bvh_from_visible(const ViewContext &vc,
                                                 const Object &object,
                                                 const GreasePencil &grease_pencil,
                                                 Span<MutableDrawingInfo> drawings,
                                                 const int frame_number)
{
  Curves2DBVHTree data;

  /* Upper bound for line count. Arrays are sized for easy index mapping, exact count isn't
   * necessary. Not all points are added to the BVH tree. */
  int max_bvh_lines = 0;
  for (const int i_drawing : drawings.index_range()) {
    if (drawings[i_drawing].frame_number == frame_number) {
      max_bvh_lines += drawings[i_drawing].drawing.strokes().evaluated_points_num();
    }
  }

  data.tree = BLI_bvhtree_new(max_bvh_lines, 0.0f, 4, 6);
  data.start_positions.reinitialize(max_bvh_lines);
  data.end_positions.reinitialize(max_bvh_lines);
  /* Compute offsets array in advance. */
  data.drawing_offsets.reinitialize(drawings.size() + 1);
  for (const int i_drawing : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[i_drawing];
    data.drawing_offsets[i_drawing] = (drawings[i_drawing].frame_number == frame_number ?
                                           info.drawing.strokes().evaluated_points_num() :
                                           0);
  }
  OffsetIndices bvh_elements_by_drawing = offset_indices::accumulate_counts_to_offsets(
      data.drawing_offsets);

  /* Insert a line for each point except end points. */
  for (const int i_drawing : drawings.index_range()) {
    const MutableDrawingInfo &info = drawings[i_drawing];
    if (drawings[i_drawing].frame_number != frame_number) {
      continue;
    }

    const bke::greasepencil::Layer &layer = grease_pencil.layer(info.layer_index);
    const float4x4 layer_to_world = layer.to_world_space(object);
    const float4x4 projection = ED_view3d_ob_project_mat_get_from_obmat(vc.rv3d, layer_to_world);
    const bke::CurvesGeometry &curves = info.drawing.strokes();
    const OffsetIndices evaluated_points_by_curve = curves.evaluated_points_by_curve();
    const VArray<bool> cyclic = curves.cyclic();
    const Span<float3> evaluated_positions = curves.evaluated_positions();
    const IndexMask curves_mask = curves.curves_range();

    /* Range of indices in the BVH tree for this drawing. */
    const IndexRange bvh_index_range = bvh_elements_by_drawing[i_drawing];
    const MutableSpan<float2> start_positions = data.start_positions.as_mutable_span().slice(
        bvh_index_range);
    const MutableSpan<float2> end_positions = data.end_positions.as_mutable_span().slice(
        bvh_index_range);

    curves_mask.foreach_index([&](const int i_curve) {
      const bool is_cyclic = cyclic[i_curve];
      const IndexRange evaluated_points = evaluated_points_by_curve[i_curve];

      /* Compute screen space positions. */
      for (const int i_point : evaluated_points) {
        const float2 co = ED_view3d_project_float_v2_m4(
            vc.region, evaluated_positions[i_point], projection);
        start_positions[i_point] = co;

        /* Last point is only valid for cyclic curves, gets ignored for non-cyclic curves. */
        const int i_prev_point = (i_point > 0 ? i_point - 1 : evaluated_points.last());
        end_positions[i_prev_point] = co;
      }

      for (const int i_point : evaluated_points.drop_back(1)) {
        const float2 &start = start_positions[i_point];
        const float2 &end = end_positions[i_point];

        const float bb[6] = {start.x, start.y, 0.0f, end.x, end.y, 0.0f};
        BLI_bvhtree_insert(data.tree, bvh_index_range[i_point], bb, 2);
      }
      /* Last->first point segment only used for cyclic curves. */
      if (is_cyclic) {
        const float2 &start = start_positions.last();
        const float2 &end = end_positions.first();

        const float bb[6] = {start.x, start.y, 0.0f, end.x, end.y, 0.0f};
        BLI_bvhtree_insert(data.tree, bvh_index_range[evaluated_points.last()], bb, 2);
      }
    });
  }

  BLI_bvhtree_balance(data.tree);

  return data;
}

void free_curves_2d_bvh_data(Curves2DBVHTree &data)
{
  if (data.tree) {
    BLI_bvhtree_free(data.tree);
    data.tree = nullptr;
  }
  data.drawing_offsets.reinitialize(0);
  data.start_positions.reinitialize(0);
  data.end_positions.reinitialize(0);
}

void find_curve_intersections(const bke::CurvesGeometry &curves,
                              const IndexMask &curve_mask,
                              const Span<float2> screen_space_positions,
                              const Curves2DBVHTree &tree_data,
                              const IndexRange tree_data_range,
                              MutableSpan<bool> r_hits,
                              std::optional<MutableSpan<float>> r_first_intersect_factors,
                              std::optional<MutableSpan<float>> r_last_intersect_factors)
{
  /* Insert segments for cutting extensions on stroke intersection. */
  const OffsetIndices points_by_curve = curves.points_by_curve();
  const VArray<bool> cyclic = curves.cyclic();

  struct RaycastArgs {
    const Curves2DBVHTree &tree_data;
    /* Indices that need to be ignored to avoid intersecting a line with itself or its immediate
     * neighbors. */
    int ignore_index1;
    int ignore_index2;
    int ignore_index3;
  };
  BVHTree_RayCastCallback callback =
      [](void *userdata, int index, const BVHTreeRay *ray, BVHTreeRayHit *hit) {
        using Result = math::isect_result<float2>;

        const RaycastArgs &args = *static_cast<const RaycastArgs *>(userdata);
        if (ELEM(index, args.ignore_index1, args.ignore_index2, args.ignore_index3)) {
          return;
        }

        const float2 ray_start = float2(ray->origin);
        const float2 ray_end = ray_start + float2(ray->direction) * ray->radius;
        const float2 &line_start = args.tree_data.start_positions[index];
        const float2 &line_end = args.tree_data.end_positions[index];
        Result result = math::isect_seg_seg(ray_start, ray_end, line_start, line_end);
        if (result.kind <= 0) {
          return;
        }
        const float dist = result.lambda * math::distance(ray_start, ray_end);
        if (dist >= hit->dist) {
          return;
        }
        /* These always need to be calculated for the BVH traversal function. */
        hit->index = index;
        hit->dist = result.lambda * math::distance(ray_start, ray_end);
        /* Don't need the hit point, only the lambda. */
        hit->no[0] = result.lambda;
      };

  /* Ray-cast in the forward direction. Ignores intersections with neighboring lines. */
  auto do_raycast = [&](const int index_back,
                        const int index,
                        const int index_forward,
                        float &r_lambda) -> bool {
    if (index_forward < 0) {
      return false;
    }

    const float2 start = screen_space_positions[index];
    const float2 end = screen_space_positions[index_forward];
    float length;
    const float2 dir = math::normalize_and_get_length(end - start, length);

    RaycastArgs args = {tree_data,
                        index_back >= 0 ? int(tree_data_range[index_back]) : -1,
                        int(tree_data_range[index]),
                        index_forward >= 0 ? int(tree_data_range[index_forward]) : -1};
    BVHTreeRayHit hit;
    hit.index = -1;
    hit.dist = FLT_MAX;
    BLI_bvhtree_ray_cast(
        tree_data.tree, float3(start, 0.0f), float3(dir, 0.0f), length, &hit, callback, &args);

    if (hit.index >= 0) {
      r_lambda = hit.no[0];
      return true;
    }
    return false;
  };

  r_hits.fill(false);
  if (r_first_intersect_factors) {
    r_first_intersect_factors->fill(-1.0f);
  }
  if (r_last_intersect_factors) {
    r_last_intersect_factors->fill(-1.0f);
  }

  curve_mask.foreach_index(GrainSize(1024), [&](const int i_curve) {
    const bool is_cyclic = cyclic[i_curve];
    const IndexRange points = points_by_curve[i_curve];

    for (const int i_point : points) {
      const int i_prev_point = (i_point == points.first() ? (is_cyclic ? points.last() : -1) :
                                                            i_point - 1);
      const int i_next_point = (i_point == points.last() ? (is_cyclic ? points.first() : -1) :
                                                           i_point + 1);
      float lambda;
      /* Find first intersections by raycast from each point to the next. */
      if (do_raycast(i_prev_point, i_point, i_next_point, lambda)) {
        r_hits[i_point] = true;
        if (r_first_intersect_factors) {
          (*r_first_intersect_factors)[i_point] = lambda;
        }
      }
      /* Find last intersections by raycast from each point to the previous. */
      if (do_raycast(i_next_point, i_point, i_prev_point, lambda)) {
        /* Note: factor = (1 - lambda) because of reverse raycast. */
        if (r_last_intersect_factors) {
          (*r_last_intersect_factors)[i_point] = 1.0f - lambda;
        }
      }
    }
  });
}

CurveSegmentsData find_curve_segments(const bke::CurvesGeometry &curves,
                                      const IndexMask &curve_mask,
                                      const Span<float2> screen_space_positions,
                                      const Curves2DBVHTree &tree_data,
                                      const IndexRange tree_data_range)
{
  const OffsetIndices points_by_curve = curves.points_by_curve();
  const VArray<bool> cyclic = curves.cyclic();

  Array<bool> hits(curves.points_num());
  Array<float> first_hit_factors(curves.points_num());
  Array<float> last_hit_factors(curves.points_num());
  find_curve_intersections(curves,
                           curve_mask,
                           screen_space_positions,
                           tree_data,
                           tree_data_range,
                           hits,
                           first_hit_factors,
                           last_hit_factors);

  IndexMaskMemory memory;
  const IndexMask hit_mask = IndexMask::from_bools(hits, memory);

  /* Count number of segments in each curve.
   * This is needed to write to the correct segments range for each curve. */
  CurveSegmentsData result;
  result.segment_offsets.reinitialize(curves.curves_num() + 1);
  /* Only segments with hits are written to, initialize all to zero. */
  result.segment_offsets.fill(0);
  curve_mask.foreach_index(GrainSize(512), [&](const int curve_i) {
    const IndexRange points = points_by_curve[curve_i];
    const IndexMask curve_hit_mask = hit_mask.slice_content(points);
    const bool is_cyclic = cyclic[curve_i];

    /* Each hit splits a segment in two. Non-cyclic curves add the curve start point as a segment
     * start point. */
    result.segment_offsets[curve_i] = (is_cyclic ? 0 : 1) + curve_hit_mask.size();
  });
  const OffsetIndices segments_by_curve = offset_indices::accumulate_counts_to_offsets(
      result.segment_offsets);

  const int num_segments = segments_by_curve.total_size();
  result.segment_start_points.reinitialize(num_segments);
  result.segment_start_fractions.reinitialize(num_segments);

  curve_mask.foreach_index(GrainSize(512), [&](const int curve_i) {
    const IndexRange points = points_by_curve[curve_i];
    const IndexMask curve_hit_mask = hit_mask.slice_content(points);
    const bool is_cyclic = cyclic[curve_i];
    const IndexRange segments = segments_by_curve[curve_i];
    const int hit_segments_start = (is_cyclic ? 0 : 1);

    if (segments.is_empty()) {
      return;
    }

    /* Add curve start a segment. */
    if (!is_cyclic) {
      result.segment_start_points[segments[0]] = points.first();
      result.segment_start_fractions[segments[0]] = 0.0f;
    }

    curve_hit_mask.foreach_index([&](const int point_i, const int hit_i) {
      result.segment_start_points[segments[hit_segments_start + hit_i]] = point_i;
      result.segment_start_fractions[segments[hit_segments_start + hit_i]] =
          first_hit_factors[point_i];
    });
  });

  return result;
}

}  // namespace blender::ed::greasepencil
