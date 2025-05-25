/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"

#include "GEO_abstract_kd_bucket_hierarchy.hh"

#include "GEO_bounding_sphere.hh"

namespace blender::geometry::bounding {

std::pair<float3, float> min_packing_sphere(const Span<float3> points)
{
  const auto search_other = [&](const float3 start_point) {
    float distance = 0.0f;
    float3 position = start_point;
    for (const float3 point : points) {
      const float new_distance = math::distance(start_point, point);
      if (distance <= new_distance) {
        position = point;
        distance = new_distance;
      }
    }
    return position;
  };

  const float3 start_b = search_other(points.first());
  const float3 start_c = search_other(start_b);

  float3 centre = math::midpoint(start_b, start_c);
  float radius = math::distance(centre, start_c);
  for (const float3 point : points) {
    const float new_radius = math::distance(centre, point);
    if (new_radius <= radius) {
      continue;
    }

    centre = math::midpoint(centre + math::normalize(centre - point) * radius, point);
    radius = (radius + new_radius) * 0.5f;
  }

  return std::pair<float3, float>(centre, radius);
}

std::pair<float3, float> concatenate_spheres(const float3 a_centre,
                                             const float a_radius,
                                             const float3 b_centre,
                                             const float b_radius)
{
  const float3 segment = math::normalize(a_centre - b_centre);
  const float3 a_extremum = a_centre + segment * a_radius;
  const float3 b_extremum = b_centre - segment * b_radius;
  return {math::midpoint(a_extremum, b_extremum), math::distance(a_extremum, b_extremum) * 0.5f};
}

void joints_packing_spheres(const OffsetIndices<int> buckets_offsets,
                            const int total_depth,
                            const Span<float3> src_bucket_points,
                            MutableSpan<float3> dst_joints_centre,
                            MutableSpan<float> dst_joints_radii)
{
  geometry::akdbh::for_each_leaf(
      buckets_offsets,
      total_depth,
      GrainSize(4096),
      [&](const IndexRange bucket_range, const int joint_index, const int /*depth_i*/) {
        if (bucket_range.is_empty()) {
          dst_joints_centre[joint_index] = float3(0);
          dst_joints_radii[joint_index] = 0.0f;
          return;
        }
        const auto [centre, radius] = min_packing_sphere(src_bucket_points.slice(bucket_range));
        dst_joints_centre[joint_index] = centre;
        dst_joints_radii[joint_index] = radius;
      });

  geometry::akdbh::for_each_to_top(buckets_offsets,
                                   total_depth,
                                   GrainSize(4096),
                                   [&](const IndexRange buckets_range,
                                       const int joint_index,
                                       const int2 /*sub_joints*/,
                                       const int /*depth_i*/) {
                                     if (buckets_range.is_empty()) {
                                       dst_joints_centre[joint_index] = float3(0);
                                       dst_joints_radii[joint_index] = 0.0f;
                                       return;
                                     }
                                     const auto [centre, radius] = min_packing_sphere(
                                         src_bucket_points.slice(buckets_range));
                                     dst_joints_centre[joint_index] = centre;
                                     dst_joints_radii[joint_index] = radius;
                                   });
}

void joints_packing_spheres_fast(const OffsetIndices<int> buckets_offsets,
                                 const int total_depth,
                                 const Span<float3> src_bucket_points,
                                 MutableSpan<float3> dst_joints_centre,
                                 MutableSpan<float> dst_joints_radii)
{
  geometry::akdbh::for_each_leaf(
      buckets_offsets,
      total_depth,
      GrainSize(4096),
      [&](const IndexRange bucket_range, const int joint_index, const int /*depth_i*/) {
        const auto [centre, radius] = min_packing_sphere(src_bucket_points.slice(bucket_range));
        dst_joints_centre[joint_index] = centre;
        dst_joints_radii[joint_index] = radius;
      });

  geometry::akdbh::for_each_to_top(buckets_offsets,
                                   total_depth,
                                   GrainSize(4096),
                                   [&](const IndexRange /*buckets_range*/,
                                       const int joint_index,
                                       const int2 sub_joints,
                                       const int /*depth_i*/) {
                                     const auto [centre, radius] = concatenate_spheres(
                                         dst_joints_centre[sub_joints[0]],
                                         dst_joints_radii[sub_joints[0]],
                                         dst_joints_centre[sub_joints[1]],
                                         dst_joints_radii[sub_joints[1]]);
                                     dst_joints_centre[joint_index] = centre;
                                     dst_joints_radii[joint_index] = radius;
                                   });
}

}  // namespace blender::geometry::bounding
