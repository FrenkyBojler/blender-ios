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
#include "BLI_sort.hh"
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
        std::nth_element(segment.begin(),
                         segment.begin() + segment.size() / 2,
                         segment.end(),
                         [&](const int a, const int b) {
                           if (UNLIKELY(positions[a][axis_index] == positions[b][axis_index])) {
                             return a < b;
                           }
                           return positions[a][axis_index] < positions[b][axis_index];
                         });
      });
}

void from_positions_non_uniform(const Span<float3> positions,
                                const int total_depth,
                                MutableSpan<int> buckets_offsets,
                                MutableSpan<int> indices,
                                const float index_w,
                                const float distance_w)
{
  BLI_assert(positions.size() == indices.size());
  BLI_assert(buckets_offsets.size() == akdbh::total_buckets_for(total_depth) + 1);

#ifndef NDEBUG
  buckets_offsets.fill(-1);
  
  std::atomic<int> counter = 2;
#endif

  buckets_offsets.first() = 0;
  buckets_offsets.last() = indices.size();
  array_utils::fill_index_range<int>(indices);

  for (const int depth_i : IndexRange(total_depth).drop_back(1)) {
    const int axis_index = math::mod_periodic(depth_i, 3);
    const IndexRange joints_range = joints_range_at_depth(depth_i);
    threading::parallel_for(joints_range.index_range(), 1024, [&](const IndexRange range) {
    for (const int joint_i : range) {
      const IndexRange joint_buckets = joint_buckets_range_at_depth(total_depth, depth_i, joint_i);

      const int first_offset = buckets_offsets[joint_buckets.start()];
      const int end_offset = buckets_offsets[joint_buckets.one_after_last()];
      BLI_assert(first_offset != -1);
      BLI_assert(end_offset != -1);
      BLI_assert(first_offset <= end_offset);

      BLI_assert(joint_buckets.start() == joint_buckets_range_at_depth(total_depth, depth_i + 1, joint_i * 2 + 0).start());
      BLI_assert(joint_buckets.one_after_last() == joint_buckets_range_at_depth(total_depth, depth_i + 1, joint_i * 2 + 1).one_after_last());

      const int child_offset = joint_buckets_range_at_depth(total_depth, depth_i + 1, joint_i * 2 + 0).one_after_last();
      BLI_assert(joint_buckets.start() <= child_offset);
      BLI_assert(child_offset <= joint_buckets.one_after_last());
      BLI_assert(buckets_offsets[child_offset] == -1);

      MutableSpan<int> joint_buckets_indices = indices.slice(IndexRange::from_begin_end(first_offset, end_offset));
      if (joint_buckets_indices.is_empty()) {
        buckets_offsets[child_offset] = end_offset;
#ifndef NDEBUG
        counter++;
#endif
        continue;
      }

      parallel_sort(joint_buckets_indices.begin(),
                    joint_buckets_indices.end(),
                    [&](const int a, const int b) {
                      if (UNLIKELY(positions[a][axis_index] == positions[b][axis_index])) {
                        return a < b;
                      }
                      return positions[a][axis_index] < positions[b][axis_index];
                    });

      BLI_assert(!joint_buckets_indices.is_empty());
      const int *centre = std::max_element(
          joint_buckets_indices.begin(),
          joint_buckets_indices.end() - 1,
          [&](const int &a, const int &b) {
            const int a_index = std::distance(joint_buckets_indices.as_span().data(), &a);
            const int b_index = std::distance(joint_buckets_indices.as_span().data(), &b);

            const float a_segment =
                positions[joint_buckets_indices[a_index + 1]][axis_index] -
                positions[a][axis_index];
            const float b_segment =
                positions[joint_buckets_indices[b_index + 1]][axis_index] -
                positions[b][axis_index];

            const double a_factor =
                0.5 -
                math::abs<double>((a_index / double(joint_buckets_indices.size())) - 0.5);
            const double b_factor =
                0.5 -
                math::abs<double>((b_index / double(joint_buckets_indices.size())) - 0.5);

            return math::pow<double>(a_factor, 1.0f) * math::pow(a_segment, 1.0f)  < math::pow<double>(b_factor, 1.0f) * math::pow(b_segment, 1.0f);
          });

      const int64_t middle_index = std::distance(joint_buckets_indices.as_span().data(), centre);
      BLI_assert(middle_index >= 0);
      const int safe_middle_index = math::clamp(middle_index, joint_buckets_indices.size() / 4, joint_buckets_indices.size() / 4 * 3);
      buckets_offsets[child_offset] = first_offset + safe_middle_index;
#ifndef NDEBUG
      counter++;
#endif
    }
    }, threading::detail::TaskSizeHints_Static(joint_size_at_depth(total_depth, depth_i) * min_bucket_size));
  }

  BLI_assert(*std::max_element(buckets_offsets.begin(), buckets_offsets.end()) <= indices.size());
  BLI_assert(0 <= *std::min_element(buckets_offsets.begin(), buckets_offsets.end()));
  BLI_assert(std::is_sorted(buckets_offsets.begin(), buckets_offsets.end()));
  BLI_assert(counter == buckets_offsets.size());
  BLI_assert(!buckets_offsets.contains(-1));
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
