/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_cpp_type.hh"

#include "BLI_array_utils.hh"
#include "BLI_generic_span.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task_size_hints.hh"

#include "BLI_bit_group_vector.hh"
#include "BLI_bit_span.hh"
#include "BLI_bit_span_ops.hh"
#include "BLI_bit_vector.hh"

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

template<typename LeafFuncT, typename JointPredicateT, typename JointFuncT>
inline void batch_for_each_to_bottom_skip(const OffsetIndices<int> buckets_offsets,
                                          const int total_depth,
                                          const IndexRange batch_range,
                                          const JointPredicateT &joint_predicate,
                                          const JointFuncT &joint_func,
                                          const LeafFuncT &leaf_func)
{
  Array<int, 0> batch_indices(batch_range.size());
  array_utils::fill_index_range<int>(batch_indices, batch_range.start());

  Vector<int, 32> depth_stack({0});
  Vector<int, 32> joint_stack({0});
  Vector<int, 32> prefix_to_visit_stack({int(batch_indices.size())});

  while (!depth_stack.is_empty()) {
    const int prefix_to_visit = prefix_to_visit_stack.pop_last();
    const int depth_i = depth_stack.pop_last();
    const int joint_i = joint_stack.pop_last();
    const MutableSpan<int> batch_to_visit = batch_indices.as_mutable_span().take_front(
        prefix_to_visit);
    const IndexRange joints_range = akdbh::joints_range_at_depth(depth_i);

    // std::sort(batch_to_visit.begin(), batch_to_visit.end());

    const auto end_of_batch_prefix = std::stable_partition(
        batch_to_visit.begin(), batch_to_visit.end(), [&](const int batch_i) -> bool {
          return joint_predicate(int(joints_range[joint_i]), batch_i);
        });

    const int num_to_visit_next = std::distance(batch_to_visit.begin(), end_of_batch_prefix);
    const Span<int> finished_batch_indices = batch_to_visit.drop_front(num_to_visit_next);
    joint_func(int(joints_range[joint_i]), finished_batch_indices);

    const Span<int> next_batch_indices = batch_to_visit.take_front(num_to_visit_next);
    if (next_batch_indices.is_empty()) {
      continue;
    }

    if (depth_i == total_depth - 1) {
      leaf_func(buckets_offsets[joint_i], next_batch_indices);
      continue;
    }

    depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
    joint_stack.extend_unchecked({joint_i * 2 + 1, joint_i * 2 + 0});
    prefix_to_visit_stack.extend_unchecked({num_to_visit_next, num_to_visit_next});
  }
}

template<typename LeafFuncT, typename JointPredicateT, typename JointFuncT>
inline void batch_for_each_to_bottom_skip_(const OffsetIndices<int> buckets_offsets,
                                           const int total_depth,
                                           const IndexRange batch_range,
                                           const JointPredicateT &joint_predicate,
                                           const JointFuncT &joint_func,
                                           const LeafFuncT &leaf_func)
{
  using namespace blender::bits;

  BitGroupVector masks_stack(33, batch_range.size(), false);
  masks_stack[0].fill(true);

  BitVector<0> buffer(batch_range.size());

  Vector<int, 32> depth_stack({0});
  Vector<int, 32> joint_stack({0});
  Vector<int, 32> mask_index_stack({0});

  while (!depth_stack.is_empty()) {
    const int depth_i = depth_stack.pop_last();
    const int joint_i = joint_stack.pop_last();
    const int mask_index = mask_index_stack.pop_last();

    const MutableBoundedBitSpan batch_mask = masks_stack[mask_index];
    const IndexRange joints_range = akdbh::joints_range_at_depth(depth_i);

    buffer.fill(false);
    foreach_1_index(batch_mask, [&](const int i) {
      if (!joint_predicate(int(joints_range[joint_i]), batch_range[i])) {
        buffer[i].set();
      }
    });

    joint_func(int(joints_range[joint_i]), buffer);

    mix_into_first_expr(
        [](const BitInt parent, const BitInt current_ended) { return parent & (~current_ended); },
        batch_mask,
        buffer);
    if (!any_bit_set(batch_mask)) {
      continue;
    }

    if (depth_i == total_depth - 1) {
      leaf_func(buckets_offsets[joint_i], batch_mask);
      continue;
    }

    MutableBoundedBitSpan other_child = masks_stack[mask_index + 1];
    other_child.fill(false);
    copy_from_or(other_child, batch_mask);

    BLI_assert(!mask_index_stack.as_span().contains(mask_index + 0));
    BLI_assert(!mask_index_stack.as_span().contains(mask_index + 1));
    mask_index_stack.extend_unchecked({mask_index + 0, mask_index + 1});
    depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
    joint_stack.extend_unchecked({joint_i * 2 + 1, joint_i * 2 + 0});
  }
}

void from_positions(Span<float3> positions,
                    OffsetIndices<int> buckets_offsets,
                    int total_depth,
                    MutableSpan<int> indices);

void from_positions_non_uniform(const Span<float3> positions,
                                const int total_depth,
                                MutableSpan<int> buckets_offsets,
                                MutableSpan<int> indices,
                                float index_w,
                                float distance_w);

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

static constexpr int32_t x_axis_stack_mask = 0b10010010'01001001'00100100'10010010;
static constexpr int32_t y_axis_stack_mask = 0b01001001'00100100'10010010'01001001;
static constexpr int32_t z_axis_stack_mask = 0b00100100'10010010'01001001'00100100;

inline uint32_t to_mask(const bool value)
{
  return value ? ~uint32_t(0) : uint32_t(0);
}

inline uint32_t joint_i_to_stack(const int depth_i, const int joint_i)
{
  BLI_assert(depth_i > 0);
  BLI_assert(joint_i >= 0);
  return uint32_t(joint_i) << (32 - depth_i);
}

inline int stack_mask(const int depth_i)
{
  BLI_assert(depth_i > 0);
  return ~((1 << (32 - depth_i)) - 1);
}

inline int stack_to_i(const int depth_i, const uint32_t stack)
{
  BLI_assert(depth_i > 0);
  return int(stack >> (32 - depth_i));
}

inline uint32_t one_way_stack(const bool x_axis, const bool y_axis, const bool z_axis)
{
  const uint32_t x_axis_stack = x_axis_stack_mask & to_mask(x_axis);
  const uint32_t y_axis_stack = y_axis_stack_mask & to_mask(y_axis);
  const uint32_t z_axis_stack = z_axis_stack_mask & to_mask(z_axis);

  return x_axis_stack | y_axis_stack | z_axis_stack;
}

uint32_t stack_from_highest_diff(uint32_t stack_a, uint32_t stack_b);

inline uint32_t merge_stacks(const int depth_i, const uint32_t prefix, const uint32_t postrix)
{
  const uint32_t prefix_mask = stack_mask(depth_i);
  return (prefix & prefix_mask) | (postrix & ~prefix_mask);
}

inline uint32_t stack_switch_branch(const uint32_t stack, const int depth_i)
{
  BLI_assert(depth_i > 0);
  return stack ^ (1 << (32 - depth_i));
}

}  // namespace blender::geometry::akdbh
