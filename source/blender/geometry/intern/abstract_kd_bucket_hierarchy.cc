/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_array_utils.hh"
#include "BLI_binary_search.hh"
#include "BLI_function_ref.hh"
#include "BLI_generic_span.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_base.hh"
#include "BLI_math_bits.h"
#include "BLI_task.hh"
#include "BLI_task_size_hints.hh"
#include "BLI_virtual_array.hh"

#include "GEO_abstract_kd_bucket_hierarchy.hh"

namespace blender::geometry::akdbh {

int total_depth_from_total(const int total_elements)
{
  int levels = 1;
  for (int total_iter = total_elements; total_iter > min_bucket_size; total_iter = total_iter / 2)
  {
    levels++;
  }
  return levels;
}

int total_joints_at_start(const int depth_i)
{
  return int((int64_t(1) << (depth_i)) - 1);
}

int total_joints_for_depth(const int total_depth)
{
  BLI_assert(total_depth > 0);
  return total_joints_at_start(total_depth);
}

int total_joints_at_depth(const int depth_i)
{
  return int(1 << depth_i);
}

int joint_size_at_depth(const int total_depth, const int depth_i)
{
  BLI_assert(total_depth > 0);
  return int(1 << (total_depth - depth_i - 1));
}

int joint_index_at_depth(const int depth_i, const int joint_i)
{
  BLI_assert(IndexRange(total_joints_at_depth(depth_i)).contains(joint_i));
  return total_joints_at_start(depth_i) + joint_i;
}

int total_buckets_at(const int depth_i)
{
  return int(1 << depth_i);
}

int total_buckets_for(const int total_depth)
{
  BLI_assert(total_depth > 0);
  return int(1 << (total_depth - 1));
}

IndexRange joints_range_at_depth(const int depth_i)
{
  return IndexRange::from_begin_size(total_joints_at_start(depth_i),
                                     total_joints_at_depth(depth_i));
}

IndexRange joint_buckets_range_at_depth(const int total_depth,
                                        const int depth_i,
                                        const int joint_i)
{
  BLI_assert(total_depth > 0);
  const int joint_size = joint_size_at_depth(total_depth, depth_i);
  return IndexRange::from_begin_size(joint_size * joint_i, joint_size);
}

OffsetIndices<int> fill_bucket_offsets_trivial(const int total_elements,
                                               MutableSpan<int> r_offsets)
{
  for (const int64_t i : r_offsets.index_range().drop_back(1)) {
    r_offsets[i] = i * int64_t(total_elements) / (r_offsets.size() - 1);
  }
  r_offsets.last() = total_elements;
  return r_offsets.as_span();
}

void from_positions(const Span<float3> positions,
                    const OffsetIndices<int> buckets_offsets,
                    const int total_depth,
                    MutableSpan<int> indices)
{
  array_utils::fill_index_range<int>(indices);

  for_each_to_bottom(
      buckets_offsets,
      total_depth,
      GrainSize(4096),
      [&](const IndexRange bucket_range, const int /*joint_index*/, const int depth_i) {
        const int axis_index = math::mod_periodic(depth_i, 3);
        MutableSpan<int> segment = indices.slice(bucket_range);
        std::nth_element(segment.begin(), segment.begin() + segment.size() / 2, segment.end(), [&](const int a, const int b) {
          if (UNLIKELY(positions[a][axis_index] == positions[b][axis_index])) {
            return a < b;
          }
          return positions[a][axis_index] < positions[b][axis_index];
        });
      });
}

void mean_sums(const OffsetIndices<int> buckets_offsets,
               const int total_depth,
               const GSpan src_buckets_data,
               GMutableSpan dst_joints_data)
{
  BLI_assert(src_buckets_data.type() == dst_joints_data.type());
  blender::geometry::akdbh::to_static_type(src_buckets_data.type(), [&](auto dummy) {
    using T = decltype(dummy);

    const Span<T> typed_src_buckets_data = src_buckets_data.typed<T>();
    MutableSpan<T> typed_dst_joints_data = dst_joints_data.typed<T>();
    for_each_leaf(
        buckets_offsets,
        total_depth,
        GrainSize(4096),
        [&](const IndexRange bucket_range, const int joint_index, const int /*depth_i*/) {
          const Span<T> bucket = typed_src_buckets_data.slice(bucket_range);
          typed_dst_joints_data[joint_index] = std::accumulate(bucket.begin(), bucket.end(), T(0));
        });

    for_each_to_top(buckets_offsets,
                    total_depth,
                    GrainSize(4096),
                    [&](const IndexRange /*buckets_range*/,
                        const int joint_index,
                        const int2 sub_joints,
                        const int /*depth_i*/) {
                      typed_dst_joints_data[joint_index] = typed_dst_joints_data[sub_joints[0]] +
                                                           typed_dst_joints_data[sub_joints[1]];
                    });
  });
}

void normalize_for_size(const OffsetIndices<int> buckets_offsets,
                        const int total_depth,
                        GMutableSpan dst_joints_data)
{
  blender::geometry::akdbh::to_static_type(dst_joints_data.type(), [&](auto dummy) {
    using T = decltype(dummy);
    MutableSpan<T> typed_dst_joints_data = dst_joints_data.typed<T>();
    for_each_leaf(
        buckets_offsets,
        total_depth,
        GrainSize(4096),
        [&](const IndexRange bucket_range, const int joint_index, const int /*depth_i*/) {
          typed_dst_joints_data[joint_index] *= math::rcp(double(bucket_range.size()));
        });
    for_each_to_bottom(
        buckets_offsets,
        total_depth,
        GrainSize(4096),
        [&](const IndexRange bucket_range, const int joint_index, const int /*nesting_i*/) {
          typed_dst_joints_data[joint_index] *= math::rcp(double(bucket_range.size()));
        });
  });
}

void accumulate_size(const OffsetIndices<int> buckets_offsets,
                     const int total_depth,
                     MutableSpan<int> dst_joints_data)
{
  for_each_leaf(buckets_offsets,
                total_depth,
                GrainSize(4096),
                [&](const IndexRange bucket_range, const int joint_index, const int /*depth_i*/) {
                  dst_joints_data[joint_index] = bucket_range.size();
                });

  for_each_to_bottom(
      buckets_offsets,
      total_depth,
      GrainSize(4096),
      [&](const IndexRange bucket_range, const int joint_index, const int /*nesting_i*/) {
        dst_joints_data[joint_index] = bucket_range.size();
      });
}

uint32_t stack_from_highest_diff(const uint32_t stack_a, const uint32_t stack_b)
{
  const uint32_t stack_diff = stack_a ^ stack_b;

  constexpr uint32_t never_empty_diff = 1;
  const uint32_t x_axis_diff_mask = highest_order_bit_uint((stack_diff & x_axis_stack_mask) |
                                                           never_empty_diff);
  const uint32_t y_axis_diff_mask = highest_order_bit_uint((stack_diff & y_axis_stack_mask) |
                                                           never_empty_diff);
  const uint32_t z_axis_diff_mask = highest_order_bit_uint((stack_diff & z_axis_stack_mask) |
                                                           never_empty_diff);

  const bool x_axis_dirrection = bool(stack_a & x_axis_diff_mask);
  const bool y_axis_dirrection = bool(stack_a & y_axis_diff_mask);
  const bool z_axis_dirrection = bool(stack_a & z_axis_diff_mask);

  return one_way_stack(x_axis_dirrection, y_axis_dirrection, z_axis_dirrection);
}

}  // namespace blender::geometry::akdbh
