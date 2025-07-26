/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_cpp_type.hh"

#include "BLI_generic_span.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task.hh"
#include "BLI_task_size_hints.hh"

namespace blender::geometry::akdbh {

template<typename Func> inline void to_static_type(const CPPType &type, const Func &func)
{
  type.to_static_type_tag<float, float3>([&](auto type_tag) {
    using T = typename decltype(type_tag)::type;
    if constexpr (!std::is_same_v<T, void>) {
      func(T());
    }
    else {
      BLI_assert_unreachable();
    }
  });
}

static constexpr int min_bucket_size = 16;

int total_depth_from_total(int total_elements);

int total_joints_at_start(int depth_i);

int total_joints_for_depth(int total_depth);

int total_joints_at_depth(int depth_i);

int total_buckets_at(int depth_i);

int total_buckets_for(int total_depth);

int joint_size_at_depth(int total_depth, int depth_i);

int joint_index_at_depth(int depth_i, int joint_i);

IndexRange joints_range_at_depth(int depth_i);

IndexRange joint_buckets_range_at_depth(int total_depth, int depth_i, int joint_i);

OffsetIndices<int> fill_bucket_offsets_trivial(int total_elements, MutableSpan<int> r_offsets);

template<typename FuncT>
inline void for_each_to_bottom(const OffsetIndices<int> buckets_offsets,
                               const int total_depth,
                               const GrainSize grain_size,
                               const FuncT &func)
{
  for (const int depth_i : IndexRange(total_depth).drop_back(1)) {
    const IndexRange joints_range = joints_range_at_depth(depth_i);
    threading::parallel_for(
        joints_range.index_range(),
        grain_size.value,
        [&](const IndexRange range) {
          for (const int joint_i : range) {
            const IndexRange joint_buckets = joint_buckets_range_at_depth(
                total_depth, depth_i, joint_i);
            func(buckets_offsets[joint_buckets], joints_range[joint_i], depth_i);
          }
        },
        threading::detail::TaskSizeHints_Static(joint_size_at_depth(total_depth, depth_i) *
                                                min_bucket_size));
  }
}

template<typename FuncT>
inline void for_each_to_top(const OffsetIndices<int> buckets_offsets,
                            const int total_depth,
                            const GrainSize grain_size,
                            const FuncT &func)
{
  const IndexRange depth_range(total_depth);
  for (const int r_depth_i : depth_range.drop_front(1)) {
    const int depth_i = depth_range.last(r_depth_i);
    const IndexRange joints_range = joints_range_at_depth(depth_i);
    const IndexRange prev_joints_range = joints_range_at_depth(depth_i + 1);
    threading::parallel_for(
        joints_range.index_range(),
        grain_size.value,
        [&](const IndexRange range) {
          for (const int joint_i : range) {
            const IndexRange joint_buckets = joint_buckets_range_at_depth(
                total_depth, depth_i, joint_i);
            const int2 sub_joints = int2(prev_joints_range.start()) + int2(joint_i * 2) +
                                    int2(0, 1);
            func(buckets_offsets[joint_buckets], joints_range[joint_i], sub_joints, depth_i);
          }
        },
        threading::detail::TaskSizeHints_Static(joint_size_at_depth(total_depth, depth_i) *
                                                min_bucket_size));
  }
}

template<typename LeafFunc>
inline void for_each_leaf(const OffsetIndices<int> buckets_offsets,
                          const int total_depth,
                          const GrainSize grain_size,
                          const LeafFunc &leaf_func)
{
  const IndexRange joints_range = joints_range_at_depth(total_depth - 1);
  threading::parallel_for(
      joints_range.index_range(),
      grain_size.value,
      [&](const IndexRange range) {
        for (const int joint_i : range) {
          leaf_func(buckets_offsets[joint_i], int(joints_range[joint_i]), total_depth - 1);
        }
      },
      threading::detail::TaskSizeHints_Static(min_bucket_size));
}

void from_positions(Span<float3> positions,
                    OffsetIndices<int> buckets_offsets,
                    int total_depth,
                    MutableSpan<int> indices);

void mean_sums(OffsetIndices<int> buckets_offsets,
               int total_depth,
               GSpan src_buckets_data,
               GMutableSpan dst_joints_data);

void normalize_for_size(OffsetIndices<int> buckets_offsets,
                        int total_depth,
                        GMutableSpan dst_joints_data);

void accumulate_size(OffsetIndices<int> buckets_offsets,
                     int total_depth,
                     MutableSpan<int> dst_joints_data);

}  // namespace blender::geometry::akdbh
