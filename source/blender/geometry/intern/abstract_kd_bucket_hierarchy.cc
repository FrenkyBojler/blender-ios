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

template<typename InT, typename OutT, typename FuncT>
static void parallel_transform(const Span<InT> src,
                               const int grain_size,
                               MutableSpan<OutT> dst,
                               const FuncT func)
{
  BLI_assert(src.size() == dst.size());
  threading::parallel_for(src.index_range(), grain_size, [&](const IndexRange range) {
    const Span<InT> src_slice = src.slice(range);
    MutableSpan<OutT> dst_slice = dst.slice(range);
    std::transform(src_slice.begin(), src_slice.end(), dst_slice.begin(), func);
  });
}

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

OffsetIndices<int> fill_bucket_offsets_trivial(const int total_elements, MutableSpan<int> r_offsets)
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
        std::sort(segment.begin(), segment.end(), [&](const int a, const int b) {
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
    for_each_leaf(buckets_offsets,
                  total_depth,
                  GrainSize(4096),
                  [&](const IndexRange bucket_range, const int joint_index, const int /*depth_i*/) {
                    const Span<T> bucket = typed_src_buckets_data.slice(bucket_range);
                    typed_dst_joints_data[joint_index] = std::accumulate(
                        bucket.begin(), bucket.end(), T(0));
                  });

    for_each_to_top(buckets_offsets,
                    total_depth,
                    GrainSize(4096),
                    [&](const IndexRange /*buckets_range*/,
                        const int joint_index,
                        const int2 sub_joints,
                        const int /*depth_i*/) {
                      typed_dst_joints_data[joint_index] = typed_dst_joints_data[sub_joints[0]] + typed_dst_joints_data[sub_joints[1]];
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
    for_each_leaf(buckets_offsets,
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
  const uint32_t x_axis_diff_mask = highest_order_bit_uint(stack_diff & x_axis_stack_mask |
                                                           never_empty_diff);
  const uint32_t y_axis_diff_mask = highest_order_bit_uint(stack_diff & y_axis_stack_mask |
                                                           never_empty_diff);
  const uint32_t z_axis_diff_mask = highest_order_bit_uint(stack_diff & z_axis_stack_mask |
                                                           never_empty_diff);

  const bool x_axis_dirrection = bool(stack_a & x_axis_diff_mask);
  const bool y_axis_dirrection = bool(stack_a & y_axis_diff_mask);
  const bool z_axis_dirrection = bool(stack_a & z_axis_diff_mask);

  return one_way_stack(x_axis_dirrection, y_axis_dirrection, z_axis_dirrection);
}

#if (0)

template<typename T>
static void gather(const Span<T> src, const Span<int> indices, MutableSpan<T> dst)
{
  BLI_assert(indices.size() == dst.size());
  for (const int64_t i : dst.index_range()) {
    dst[i] = src[indices[i]];
  }
}

static void squared_distance(const float3 centre,
                             const Span<float3> positions,
                             MutableSpan<float> dst)
{
  BLI_assert(positions.size() == dst.size());
  std::transform(positions.begin(), positions.end(), dst.begin(), [&](const float3 &position) {
    return math::distance_squared(position, centre);
  });
}

static float3 accumulate_difference(const float3 value,
                                    const Span<float3> values,
                                    const Span<float> weight)
{
  BLI_assert(values.size() == weight.size());
  float3 accumulate_from_zero(0);
  for (const int index : weight.index_range()) {
    accumulate_from_zero += (values[index] - value) * weight[index];
  }
  return accumulate_from_zero;
}

struct Item {
  int depth_i;
  int joint_i;
  int prefix_to_visit;
};

template<typename LeafFuncT, typename JointPredicateT, typename JointFuncT>
static void for_each_to_bottom_skip(const OffsetIndices<int> buckets_offsets,
                                    const int total_depth,
                                    const IndexRange range,
                                    const JointPredicateT &joint_predicate,
                                    const JointFuncT &joint_func,
                                    const LeafFuncT &leaf_func)
{
  Array<int, 0> indices(range.size());
  array_utils::fill_index_range<int>(indices, range.start());
  Vector<Item, 32> stack = {Item{0, 0, int(indices.size())}};
  while (!stack.is_empty()) {
    const Item item = stack.pop_last();
    const int depth_i = item.depth_i;
    const int joint_i = item.joint_i;
    const MutableSpan<int> to_visit = indices.as_mutable_span().take_front(item.prefix_to_visit);
    const IndexRange joints_range = joints_range_at_depth(depth_i);
    const IndexRange joint_buckets = joint_buckets_range_at_depth(total_depth, depth_i, joint_i);

    const auto end_of_prefix = std::stable_partition(
        to_visit.begin(), to_visit.end(), [&](const int i) -> bool {
          return joint_predicate(int(joints_range[joint_i]), i);
        });

    const Span<int> finished_indices = to_visit.drop_front(
        std::distance(to_visit.begin(), end_of_prefix));
    joint_func(buckets_offsets[joint_buckets], int(joints_range[joint_i]), finished_indices);

    const Span<int> next_indices = to_visit.take_front(
        std::distance(to_visit.begin(), end_of_prefix));
    if (next_indices.is_empty()) {
      continue;
    }

    if (depth_i == total_depth - 1) {
      leaf_func(buckets_offsets[joint_i], next_indices);
      continue;
    }

    stack.append({depth_i + 1, joint_i * 2 + 1, int(next_indices.size())});
    stack.append({depth_i + 1, joint_i * 2 + 0, int(next_indices.size())});
  }
}

static void sample_mean_average(const OffsetIndices<int> buckets_offsets,
                                const int total_depth,
                                const Span<float3> src_joints_centre,
                                const Span<float> src_joints_min_distance,
                                const Span<float3> src_joints_value,
                                const Span<float3> src_bucket_position,
                                const Span<float3> src_bucket_value,
                                const int power_value,
                                const float offset_value,
                                MutableSpan<float3> dst_buckets_data)
{
  BLI_assert(src_joints_centre.size() == src_joints_min_distance.size());
  BLI_assert(src_bucket_value.size() == dst_buckets_data.size());
  BLI_assert(src_bucket_value.size() == src_bucket_position.size());

  const FunctionRef<void(int, MutableSpan<float>)> squared_distance_invertion =
      powered_rcp_for_squared(power_value);

  threading::parallel_for(src_bucket_value.index_range(), 1024 * 8, [&](const IndexRange range) {
    /*

      constexpr int grain_size = 1024;
      for (const int grain_i : IndexRange((src_bucket_value.size() + grain_size - 1) / grain_size))
      { const IndexRange range = src_bucket_value.index_range().drop_front(grain_i *
      grain_size).take_front(grain_size);
    */

    Vector<float> buffer;
    buffer.reserve(range.size());

    for_each_to_bottom_skip(
        buckets_offsets,
        total_depth,
        range,
        [&](const int joint_index, const int value_i) -> bool {
          return (math::distance(src_joints_centre[joint_index], src_bucket_position[value_i]) +
                  offset_value) <= src_joints_min_distance[joint_index];
        },
        [&](const IndexRange buckets_range, const int joint_index, const Span<int> value_indices) {
          buffer.resize(value_indices.size());
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            buffer[value_i] = math::square(
                math::distance(src_joints_centre[joint_index], src_bucket_position[value_index]) +
                offset_value);
          }

          squared_distance_invertion(power_value, buffer.as_mutable_span());

          const float total_factor = buckets_range.size();
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            dst_buckets_data[value_index] += src_joints_value[joint_index] * buffer[value_i] *
                                             total_factor;
          }
        },
        [&](const IndexRange bucket_range, const Span<int> value_indices) {
          buffer.resize(bucket_range.size());
          for (const int value_i : value_indices) {
            const float3 position = src_bucket_position[value_i];

            for (const int index : bucket_range.index_range()) {
              buffer[index] = math::square(
                  math::distance(src_bucket_position[bucket_range[index]], position) +
                  offset_value);
            }

            squared_distance_invertion(power_value, buffer.as_mutable_span());

            for (const int i : bucket_range.index_range()) {
              const int index = bucket_range[i];
              const float relation_factor = buffer[i];
              const float safe_relation_factor = index == value_i ? 0.0f : relation_factor;
              dst_buckets_data[value_i] += src_bucket_value[index] * safe_relation_factor;
            }
          }
        });
  });
}

static void sample_average(const OffsetIndices<int> buckets_offsets,
                           const int total_depth,
                           const Span<float3> src_joints_centre,
                           const Span<float> src_joints_min_distance,
                           const Span<float3> src_joints_value,
                           const Span<float3> src_bucket_position,
                           const Span<float3> src_bucket_value,
                           const int power_value,
                           const float offset_value,
                           MutableSpan<float3> dst_buckets_data)
{
  BLI_assert(src_joints_centre.size() == src_joints_min_distance.size());
  BLI_assert(src_bucket_value.size() == dst_buckets_data.size());
  BLI_assert(src_bucket_value.size() == src_bucket_position.size());

  const FunctionRef<void(int, MutableSpan<float>)> squared_distance_invertion =
      powered_rcp_for_squared(power_value);

  threading::parallel_for(src_bucket_value.index_range(), 1024 * 8, [&](const IndexRange range) {
    /*

      constexpr int grain_size = 1024;
      for (const int grain_i : IndexRange((src_bucket_value.size() + grain_size - 1) / grain_size))
      { const IndexRange range = src_bucket_value.index_range().drop_front(grain_i *
      grain_size).take_front(grain_size);
    */

    Vector<float> buffer;
    buffer.reserve(range.size());

    for_each_to_bottom_skip(
        buckets_offsets,
        total_depth,
        range,
        [&](const int joint_index, const int value_i) -> bool {
          return (math::distance(src_joints_centre[joint_index], src_bucket_position[value_i]) +
                  offset_value) <= src_joints_min_distance[joint_index];
        },
        [&](const IndexRange buckets_range, const int joint_index, const Span<int> value_indices) {
          buffer.resize(value_indices.size());
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            buffer[value_i] = math::square(
                math::distance(src_joints_centre[joint_index], src_bucket_position[value_index]) +
                offset_value);
          }

          squared_distance_invertion(power_value, buffer.as_mutable_span());

          const float total_factor = buckets_range.size();
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            dst_buckets_data[value_index] += (src_joints_value[joint_index] -
                                              src_bucket_value[value_index]) *
                                             buffer[value_i] * total_factor;
          }
        },
        [&](const IndexRange bucket_range, const Span<int> value_indices) {
          buffer.resize(bucket_range.size());
          for (const int value_i : value_indices) {
            const float3 position = src_bucket_position[value_i];

            for (const int index : bucket_range.index_range()) {
              buffer[index] = math::square(
                  math::distance(src_bucket_position[bucket_range[index]], position) +
                  offset_value);
            }

            squared_distance_invertion(power_value, buffer.as_mutable_span());

            const float3 self_value = src_bucket_value[value_i];
            for (const int i : bucket_range.index_range()) {
              const int index = bucket_range[i];
              const float relation_factor = buffer[i];
              const float safe_relation_factor = index == value_i ? 0.0f : relation_factor;
              dst_buckets_data[value_i] += (src_bucket_value[index] - self_value) *
                                           safe_relation_factor;
            }
          }
        });
  });
}

static void sample_gradient_average(const OffsetIndices<int> buckets_offsets,
                                    const int total_depth,
                                    const Span<float3> src_joints_centre,
                                    const Span<float> src_joints_min_distance,
                                    const Span<float3> src_joints_value,
                                    const Span<float3> src_bucket_position,
                                    const Span<float3> src_bucket_value,
                                    const int power_value,
                                    const float offset_value,
                                    MutableSpan<float3> dst_buckets_data)
{
  BLI_assert(src_joints_centre.size() == src_joints_min_distance.size());
  BLI_assert(src_bucket_value.size() == dst_buckets_data.size());
  BLI_assert(src_bucket_value.size() == src_bucket_position.size());

  const FunctionRef<void(int, MutableSpan<float>)> squared_distance_invertion =
      powered_rcp_for_squared(power_value);

  threading::parallel_for(src_bucket_value.index_range(), 1024 * 8, [&](const IndexRange range) {
    /*

      constexpr int grain_size = 1024;
      for (const int grain_i : IndexRange((src_bucket_value.size() + grain_size - 1) / grain_size))
      { const IndexRange range = src_bucket_value.index_range().drop_front(grain_i *
      grain_size).take_front(grain_size);
    */

    Vector<float> buffer;
    buffer.reserve(range.size());

    for_each_to_bottom_skip(
        buckets_offsets,
        total_depth,
        range,
        [&](const int joint_index, const int value_i) -> bool {
          return (math::distance(src_joints_centre[joint_index], src_bucket_position[value_i]) +
                  offset_value) <= src_joints_min_distance[joint_index];
        },
        [&](const IndexRange buckets_range, const int joint_index, const Span<int> value_indices) {
          buffer.resize(value_indices.size());
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            buffer[value_i] = math::square(
                math::distance(src_joints_centre[joint_index], src_bucket_position[value_index]) +
                offset_value);
          }

          squared_distance_invertion(power_value, buffer.as_mutable_span());

          const float total_factor = buckets_range.size();
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            dst_buckets_data[value_index] += math::normalize(src_joints_centre[joint_index] -
                                                             src_bucket_position[value_index]) *
                                             src_joints_value[joint_index] * buffer[value_i] *
                                             total_factor;
          }
        },
        [&](const IndexRange bucket_range, const Span<int> value_indices) {
          buffer.resize(bucket_range.size());
          for (const int value_i : value_indices) {
            const float3 position = src_bucket_position[value_i];

            for (const int i : bucket_range.index_range()) {
              const int index = bucket_range[i];
              buffer[i] = math::square(math::distance(src_bucket_position[index], position) +
                                       offset_value);
            }

            squared_distance_invertion(power_value, buffer.as_mutable_span());

            const float3 self_value = src_bucket_value[value_i];
            for (const int i : bucket_range.index_range()) {
              const int index = bucket_range[i];
              const float relation_factor = buffer[i];
              const float safe_relation_factor = index == value_i ? 0.0f : relation_factor;
              dst_buckets_data[value_i] += math::normalize(src_bucket_position[index] - position) *
                                           src_bucket_value[index] * safe_relation_factor;
            }
          }
        });
  });
}

template<typename LeafFuncT, typename JointPredicateT, typename JointFuncT>
static void for_each_to_bottom_skip_old(const OffsetIndices<int> buckets_offsets,
                                        const int total_depth,
                                        const IndexRange range,
                                        const JointPredicateT &joint_predicate,
                                        const JointFuncT &joint_func,
                                        const LeafFuncT &leaf_func)
{
  Array<int, 0> indices(range.size());
  array_utils::fill_index_range<int>(indices, range.start());
  Vector<Item, 32> stack = {Item{0, 0, int(indices.size())}};
  while (!stack.is_empty()) {
    const Item item = stack.pop_last();
    const int depth_i = item.depth_i;
    const int joint_i = item.joint_i;
    const MutableSpan<int> to_visit = indices.as_mutable_span().take_front(item.prefix_to_visit);
    const IndexRange joints_range = joints_range_at_depth(depth_i);
    const IndexRange joint_buckets = joint_buckets_range_at_depth(total_depth, depth_i, joint_i);

    const auto end_of_prefix = std::stable_partition(
        to_visit.begin(), to_visit.end(), [&](const int i) -> bool {
          return joint_predicate(int(joints_range[joint_i]), i);
        });

    const Span<int> finished_indices = to_visit.drop_front(
        std::distance(to_visit.begin(), end_of_prefix));
    joint_func(buckets_offsets[joint_buckets], int(joints_range[joint_i]), finished_indices);

    const Span<int> next_indices = to_visit.take_front(
        std::distance(to_visit.begin(), end_of_prefix));
    if (next_indices.is_empty()) {
      continue;
    }

    if (depth_i == total_depth - 1) {
      leaf_func(buckets_offsets[joint_i], next_indices);
      continue;
    }

    stack.append({depth_i + 1, joint_i * 2 + 0, int(next_indices.size())});
    stack.append({depth_i + 1, joint_i * 2 + 1, int(next_indices.size())});
  }
}

static void sample_average_old(const OffsetIndices<int> buckets_offsets,
                               const int total_depth,
                               const Span<float3> src_joints_centre,
                               const Span<float> src_joints_min_distance,
                               const Span<float3> src_joints_value,
                               const Span<float3> src_bucket_position,
                               const Span<float3> src_bucket_value,
                               const int power_value,
                               MutableSpan<float3> dst_buckets_data)
{
  BLI_assert(src_joints_centre.size() == src_joints_min_distance.size());
  BLI_assert(src_bucket_value.size() == dst_buckets_data.size());
  BLI_assert(src_bucket_value.size() == src_bucket_position.size());

  const FunctionRef<void(int, MutableSpan<float>)> squared_distance_invertion =
      powered_rcp_for_squared(power_value);

  threading::parallel_for(src_bucket_value.index_range(), 1024, [&](const IndexRange range) {
    Vector<float> buffer;
    buffer.reserve(range.size());

    for_each_to_bottom_skip_old(
        buckets_offsets,
        total_depth,
        range,
        [&](const int joint_index, const int value_i) -> bool {
          return math::distance_squared(src_joints_centre[joint_index],
                                        src_bucket_position[value_i]) <=
                 math::square(src_joints_min_distance[joint_index]);
        },
        [&](const IndexRange buckets_range, const int joint_index, const Span<int> value_indices) {
          buffer.resize(value_indices.size());
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            buffer[value_i] = math::distance_squared(src_joints_centre[joint_index],
                                                     src_bucket_position[value_index]);
          }

          squared_distance_invertion(power_value, buffer.as_mutable_span());

          const float total_factor = buckets_range.size();
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            dst_buckets_data[value_index] += (src_joints_value[joint_index] -
                                              src_bucket_value[value_index]) *
                                             buffer[value_i] * total_factor;
          }
        },
        [&](const IndexRange bucket_range, const Span<int> value_indices) {
          buffer.resize(bucket_range.size());
          for (const int value_i : value_indices) {
            const float3 position = src_bucket_position[value_i];

            for (const int index : bucket_range.index_range()) {
              buffer[index] = math::distance_squared(src_bucket_position[bucket_range[index]],
                                                     position);
            }

            squared_distance_invertion(power_value, buffer.as_mutable_span());

            const float3 self_value = src_bucket_value[value_i];
            for (const int i : bucket_range.index_range()) {
              const int index = bucket_range[i];
              const float relation_factor = buffer[i];
              const float self_ignore_relation_factor = UNLIKELY(index == value_i) ?
                                                            0.0f :
                                                            relation_factor;
              dst_buckets_data[value_i] += (src_bucket_value[index] - self_value) *
                                           self_ignore_relation_factor;
            }
          }
        });
  });
}

template<typename LeafFuncT, typename JointPredicateT, typename JointFuncT>
static void for_each_to_bottom_skip_new(const OffsetIndices<int> buckets_offsets,
                                        const int total_depth,
                                        const IndexRange range,
                                        const JointPredicateT &joint_predicate,
                                        const JointFuncT &joint_func,
                                        const LeafFuncT &leaf_func)
{
  Array<int, 0> begin_indices(range.size());
  array_utils::fill_index_range<int>(begin_indices, range.start());

  Vector<std::pair<int, Array<int, 0>>> parent_joint_indices = {{0, std::move(begin_indices)}};

  for (const int depth_i : IndexRange(total_depth)) {
    const IndexRange joints_range = joints_range_at_depth(depth_i);

    Vector<std::pair<int, Array<int, 0>>> new_joint_indices;
    new_joint_indices.reserve(new_joint_indices.size() * 2);

    for (const int joint_data_i : parent_joint_indices.index_range()) {
      const int joint_i = parent_joint_indices[joint_data_i].first;

      const IndexRange joint_buckets = joint_buckets_range_at_depth(total_depth, depth_i, joint_i);

      MutableSpan<int> parent_indices =
          parent_joint_indices[joint_data_i].second.as_mutable_span();

      const auto end_of_prefix = std::stable_partition(
          parent_indices.begin(), parent_indices.end(), [&](const int i) -> bool {
            return joint_predicate(int(joints_range[joint_i]), i);
          });

      const Span<int> finished_indices = parent_indices.drop_front(
          std::distance(parent_indices.begin(), end_of_prefix));
      const Span<int> next_indices = parent_indices.take_front(
          std::distance(parent_indices.begin(), end_of_prefix));

      joint_func(buckets_offsets[joint_buckets], int(joints_range[joint_i]), finished_indices);

      if (next_indices.is_empty()) {
        continue;
      }

      if (IndexRange(total_depth).last() == depth_i) {
        leaf_func(buckets_offsets[joint_i], next_indices);
        continue;
      }

      const int next_joint_a = joint_i * 2 + 0;
      const int next_joint_b = joint_i * 2 + 1;

      new_joint_indices.append({next_joint_a, Array<int, 0>(next_indices)});
      if (next_indices.size() == parent_indices.size()) {
        new_joint_indices.append(
            {next_joint_b, std::move(parent_joint_indices[joint_data_i].second)});
      }
      else {
        new_joint_indices.append({next_joint_b, Array<int, 0>(next_indices)});
      }
    }

    parent_joint_indices = std::move(new_joint_indices);

    if (parent_joint_indices.is_empty()) {
      break;
    }
  }
}

static void sample_average_new(const OffsetIndices<int> buckets_offsets,
                               const int total_depth,
                               const Span<float3> src_joints_centre,
                               const Span<float> src_joints_min_distance,
                               const Span<float3> src_joints_value,
                               const Span<float3> src_bucket_position,
                               const Span<float3> src_bucket_value,
                               const int power_value,
                               MutableSpan<float3> dst_buckets_data)
{
  BLI_assert(src_joints_centre.size() == src_joints_min_distance.size());
  BLI_assert(src_bucket_value.size() == dst_buckets_data.size());
  BLI_assert(src_bucket_value.size() == src_bucket_position.size());

  const FunctionRef<void(int, MutableSpan<float>)> squared_distance_invertion =
      powered_rcp_for_squared(power_value);

  threading::parallel_for(src_bucket_value.index_range(), 1024 * 16, [&](const IndexRange range) {
    Vector<float> buffer;
    buffer.reserve(range.size());

    for_each_to_bottom_skip_new(
        buckets_offsets,
        total_depth,
        range,
        [&](const int joint_index, const int value_i) -> bool {
          return math::distance_squared(src_joints_centre[joint_index],
                                        src_bucket_position[value_i]) <=
                 math::square(src_joints_min_distance[joint_index]);
        },
        [&](const IndexRange buckets_range, const int joint_index, const Span<int> value_indices) {
          buffer.resize(value_indices.size());
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            buffer[value_i] = math::distance_squared(src_joints_centre[joint_index],
                                                     src_bucket_position[value_index]);
          }

          squared_distance_invertion(power_value, buffer.as_mutable_span());

          const float total_factor = buckets_range.size();
          for (const int value_i : value_indices.index_range()) {
            const int value_index = value_indices[value_i];
            dst_buckets_data[value_index] += (src_joints_value[joint_index] -
                                              src_bucket_value[value_index]) *
                                             buffer[value_i] * total_factor;
          }
        },
        [&](const IndexRange bucket_range, const Span<int> value_indices) {
          buffer.resize(bucket_range.size());
          for (const int value_i : value_indices) {
            const float3 position = src_bucket_position[value_i];

            for (const int index : bucket_range.index_range()) {
              buffer[index] = math::distance_squared(src_bucket_position[bucket_range[index]],
                                                     position);
            }

            squared_distance_invertion(power_value, buffer.as_mutable_span());

            const float3 self_value = src_bucket_value[value_i];
            for (const int i : bucket_range.index_range()) {
              const int index = bucket_range[i];
              const float relation_factor = buffer[i];
              const float self_ignore_relation_factor = UNLIKELY(index == value_i) ?
                                                            0.0f :
                                                            relation_factor;
              dst_buckets_data[value_i] += (src_bucket_value[index] - self_value) *
                                           self_ignore_relation_factor;
            }
          }
        });
  });
}

static void parents_to_childs(const Span<int> parents,
                              const IndexRange parent_range,
                              const IndexRange child_range,
                              MutableSpan<int> childs)
{
  BLI_assert(parents.size() * 2 == childs.size());
  for (const int i : parents.index_range()) {
    const int parent_i = parents[i] - parent_range.start();
    childs[i * 2 + 0] = child_range[parent_i * 2 + 0];
    childs[i * 2 + 1] = child_range[parent_i * 2 + 1];
  }
}

template<typename JointPredicateT /*, typename BucketPredicateT*/,
         typename JointFuncT,
         typename LeafFuncT>
static void for_each_to_bottom_skip_fast(const int total_depth,
                                         const JointPredicateT &joint_predicate,
                                         // const BucketPredicateT &bucket_predicate,
                                         const JointFuncT &joint_func,
                                         const LeafFuncT &leaf_func)
{
  Vector<Array<int, 0>, 32> all_around_stack = {{}};
  Vector<int, 32> depth_stack = {0};
  Vector<int, 32> joint_i_stack = {0};

  while (!all_around_stack.is_empty()) {
    BLI_assert(all_around_stack.size() == depth_stack.size());
    BLI_assert(all_around_stack.size() == joint_i_stack.size());

    Array<int, 0> all_around_parent = all_around_stack.pop_last();
    const int depth_i = depth_stack.pop_last();
    const int joint_i = joint_i_stack.pop_last();

    const IndexRange joints_range = joints_range_at_depth(depth_i);
    const IndexRange child_joints_range = joints_range_at_depth(depth_i + 1);

    const int joint_index = joints_range[joint_i];

    const auto end_of_prefix = std::stable_partition(all_around_parent.begin(),
                                                     all_around_parent.end(),
                                                     [&](const int other_joint_index) -> bool {
                                                       return joint_predicate(joint_index,
                                                                              other_joint_index);
                                                     });

    const Span<int> all_far_parent = all_around_parent.as_span().drop_front(
        std::distance(all_around_parent.begin(), end_of_prefix));
    MutableSpan<int> all_near_parent = all_around_parent.as_mutable_span().take_front(
        std::distance(all_around_parent.begin(), end_of_prefix));

    const IndexRange joint_buckets = joint_buckets_range_at_depth(total_depth, depth_i, joint_i);
    joint_func(joint_buckets, all_far_parent);

    if (depth_i == IndexRange(total_depth).last()) {
      const IndexRange parent_joints_range = joints_range_at_depth(depth_i - 1);
      std::transform(all_near_parent.begin(),
                     all_near_parent.end(),
                     all_near_parent.begin(),
                     [&](const int joint_index) {
                       const int joint_i = joint_index - parent_joints_range.start();
                       return joint_i;
                     });
      const int self_bucket_index = joint_i;
      leaf_func(self_bucket_index, all_near_parent);
      continue;
    }

    const int left_child_i = joint_i * 2 + 0;
    const int right_child_i = joint_i * 2 + 1;

    const bool parent_has_other = depth_i > 1;

    Array<int, 0> left_child_near(all_near_parent.size() * 2 + int(parent_has_other));
    Array<int, 0> right_child_near(all_near_parent.size() * 2 + int(parent_has_other));

    parents_to_childs(all_near_parent.as_span(),
                      joints_range,
                      child_joints_range,
                      left_child_near.as_mutable_span().drop_back(int(parent_has_other)));
    right_child_near.as_mutable_span().copy_from(left_child_near.as_span());

    //  if (parent_has_other) {
    //    left_child_near.last() =
    //    right_child_near.last() =
    //  }

    left_child_near.last() = child_joints_range[right_child_i];
    right_child_near.last() = child_joints_range[left_child_i];

    all_around_stack.append(std::move(right_child_near));
    depth_stack.append(depth_i + 1);
    joint_i_stack.append(right_child_i);

    all_around_stack.append(std::move(left_child_near));
    depth_stack.append(depth_i + 1);
    joint_i_stack.append(left_child_i);
  }
}

static void sample_average_fast(const OffsetIndices<int> buckets_offsets,
                                const int total_depth,
                                const int power_value,
                                const Span<float3> joints_centre,
                                const Span<float> joints_min_radius,
                                const Span<float> joints_min_distance,
                                const Span<float3> joints_value,
                                const Span<float> joints_value_factor,
                                const Span<float3> buckets_position,
                                const Span<float3> src_buckets_value,
                                MutableSpan<float3> dst_buckets_value)
{
  BLI_assert(joints_centre.size() == joints_min_radius.size());
  BLI_assert(joints_centre.size() == joints_min_distance.size());
  BLI_assert(joints_centre.size() == joints_value.size());
  BLI_assert(dst_buckets_value.size() == buckets_position.size());
  BLI_assert(dst_buckets_value.size() == src_buckets_value.size());

  const FunctionRef<void(int, MutableSpan<float>)> squared_distance_invertion =
      powered_rcp_for_squared(power_value);

  Vector<float> factors_buffer;
  Vector<float3> buffer;
  Vector<float> joints_factors_buffer;

  const IndexRange buckets_range = joints_range_at_depth(total_depth - 1);

  for_each_to_bottom_skip_fast(
      total_depth,
      [&](const int joint_index, const int other_joint_index) -> bool {
        const float distance_squared = math::distance_squared(joints_centre[joint_index],
                                                              joints_centre[other_joint_index]);
        const int joint_min_radius = joints_min_radius[joint_index];
        const int other_joint_min_distance = joints_min_distance[other_joint_index];
        return math::square(joint_min_radius + other_joint_min_distance) > distance_squared;
      }, /*
      [&](const int joint_index, const int bucket_index) -> bool {
        const float distance_squared = math::distance_squared(joints_centre[joint_index],
      buckets_position[bucket_index]); const int joint_min_distance =
      joints_min_distance[joint_index]; return math::square(joint_min_distance) > distance_squared;
      }, */
      [&](const IndexRange buckets, const Span<int> far_joints) {
        buffer.resize(far_joints.size() * 2);
        joints_factors_buffer.resize(far_joints.size());

        gather<float>(joints_value_factor, far_joints, joints_factors_buffer.as_mutable_span());

        {
          MutableSpan<float3> buffer_joint_value = buffer.as_mutable_span().take_front(
              far_joints.size());
          MutableSpan<float3> buffer_joint_position = buffer.as_mutable_span().take_back(
              far_joints.size());
          gather<float3>(joints_value, far_joints, buffer_joint_value);
          gather<float3>(joints_centre, far_joints, buffer_joint_position);
        }

        const Span<float3> other_value = buffer.as_span().take_front(far_joints.size());
        const Span<float3> other_position = buffer.as_span().take_back(far_joints.size());

        const IndexRange bucket_range = buckets_offsets[buckets];
        const Span<float3> self_position = buckets_position.slice(bucket_range);
        const Span<float3> self_value = src_buckets_value.slice(bucket_range);
        MutableSpan<float3> dst_value = dst_buckets_value.slice(bucket_range);

        factors_buffer.resize(far_joints.size());
        for (const int index : dst_value.index_range()) {
          const float3 position = self_position[index];
          const float3 value = self_value[index];

          squared_distance(position, other_position, factors_buffer);
          squared_distance_invertion(power_value, factors_buffer.as_mutable_span());

          for (const int i : factors_buffer.index_range()) {
            factors_buffer[i] *= joints_factors_buffer[i];
          }

          dst_value[index] += accumulate_difference(value, other_value, factors_buffer);
        }
      },
      [&](const int self_joint, const Span<int> near_joints) {
        BLI_assert(!near_joints.contains(self_joint));

        const IndexRange bucket_range = buckets_offsets[self_joint];
        const Span<float3> self_position = buckets_position.slice(bucket_range);
        const Span<float3> self_value = src_buckets_value.slice(bucket_range);
        MutableSpan<float3> dst_value = dst_buckets_value.slice(bucket_range);

        for (const int index : bucket_range.index_range()) {
          const float3 position = self_position[index];
          const float3 value = self_value[index];

          float3 accumulate_from_zero(0);
          for (const int near_joint_i : near_joints) {

            //  {
            //    const int near_joint_index = buckets_range[near_joint_i];
            //    const float distance_squared = math::distance_squared(position,
            //    joints_centre[near_joint_index]); const int joint_min_distance =
            //    joints_min_distance[near_joint_index]; return math::square(joint_min_distance) >
            //    distance_squared;
            //  }

            const IndexRange near_bucket_range = buckets_offsets[near_joint_i];
            const Span<float3> other_position = buckets_position.slice(near_bucket_range);
            const Span<float3> other_value = src_buckets_value.slice(near_bucket_range);

            factors_buffer.resize(other_value.size());
            squared_distance(position, other_position, factors_buffer);
            squared_distance_invertion(power_value, factors_buffer.as_mutable_span());
            accumulate_from_zero += accumulate_difference(value, other_value, factors_buffer);
          }
          dst_value[index] += accumulate_from_zero;
        }

        factors_buffer.resize(bucket_range.size());
        for (const int self_i : bucket_range.index_range()) {
          const float3 position = self_position[self_i];
          const float3 value = self_value[self_i];

          squared_distance(position, self_position, factors_buffer);
          squared_distance_invertion(power_value, factors_buffer.as_mutable_span());
          factors_buffer[self_i] = 0.0f;
          dst_value[self_i] += accumulate_difference(value, self_value, factors_buffer);
        }
      });
}

template<typename LeafFuncT, typename JointPredicateT, typename JointFuncT>
static void for_each_to_bottom_latest(const OffsetIndices<int> buckets_offsets,
                                      const int total_depth,
                                      const int parent_depth_i,
                                      const int parent_i,
                                      const JointPredicateT &joint_predicate,
                                      const JointFuncT &joint_func,
                                      const LeafFuncT &leaf_func)
{
  BLI_assert(!buckets_offsets.is_empty());

  Vector<int, 64> joint_to_pass_depht_stack = {parent_depth_i + 1, parent_depth_i + 1};
  Vector<int, 64> joint_to_pass_i_stack = {parent_i * 2 + 1, parent_i * 2 + 0};
  Vector<int, 64> joint_to_sample_depth_stack = {parent_depth_i + 1, parent_depth_i + 1};
  Vector<int, 64> joint_to_sample_i_stack = {parent_i * 2 + 0, parent_i * 2 + 1};

  while (!joint_to_pass_depht_stack.is_empty()) {
    BLI_assert(joint_to_pass_depht_stack.size() == joint_to_pass_i_stack.size());
    BLI_assert(joint_to_pass_depht_stack.size() == joint_to_sample_depth_stack.size());
    BLI_assert(joint_to_pass_depht_stack.size() == joint_to_sample_i_stack.size());

    const int joint_to_pass_depth = joint_to_pass_depht_stack.pop_last();
    const int joint_to_pass_i = joint_to_pass_i_stack.pop_last();
    // const int joint_to_pass_index = joint_index_at_depth(joint_to_pass_depth, joint_to_pass_i);
    const uint32_t stack_to_pass = joint_i_to_stack(joint_to_pass_depth, joint_to_pass_i);

    const int joint_to_sample_depth = joint_to_sample_depth_stack.pop_last();
    const int joint_to_sample_i = joint_to_sample_i_stack.pop_last();
    const int joint_to_sample_index = joint_index_at_depth(joint_to_sample_depth,
                                                           joint_to_sample_i);
    const uint32_t stack_to_sample = joint_i_to_stack(joint_to_sample_depth, joint_to_sample_i);

    BLI_assert(joint_to_pass_depth < total_depth);
    BLI_assert(joint_to_sample_depth < total_depth);

    const uint32_t to_pass_dirrection_stack = stack_from_highest_diff(stack_to_pass,
                                                                      stack_to_sample);
    const uint32_t down_stack_to_pass = merge_stacks(
        joint_to_pass_depth, stack_to_pass, to_pass_dirrection_stack);

    const IndexRange rest_depth_to_pass = IndexRange::from_begin_end(joint_to_pass_depth,
                                                                     total_depth);
    const int rest_depth_to_pass_i = binary_search::first_if(
        rest_depth_to_pass, [&](const int depth_i) {
          const int sub_joint_i_to_pass = stack_to_i(depth_i, down_stack_to_pass);
          const int sub_joint_index_to_pass = joint_index_at_depth(depth_i, sub_joint_i_to_pass);
          return joint_predicate(sub_joint_index_to_pass, joint_to_sample_index);
        });
    const bool need_to_double_sampler = rest_depth_to_pass_i == rest_depth_to_pass.size();

    if (need_to_double_sampler) {
      if (joint_to_sample_depth == IndexRange(total_depth).last()) {
        const IndexRange buckets_to_pass = joint_buckets_range_at_depth(
            total_depth, joint_to_pass_depth, joint_to_pass_i);
        const IndexRange range_to_pass = buckets_offsets[buckets_to_pass];

        const IndexRange buckets_to_sample = joint_buckets_range_at_depth(
            total_depth, joint_to_sample_depth, joint_to_sample_i);
        const IndexRange range_to_sample = buckets_offsets[buckets_to_sample];

        leaf_func(range_to_pass, range_to_sample);
        // leaf_func(joint_index_at_depth(joint_to_pass_depth, joint_to_pass_i),
        // joint_index_at_depth(joint_to_sample_depth, joint_to_sample_i));
        continue;
      }

      joint_to_pass_depht_stack.append(joint_to_pass_depth);
      joint_to_pass_i_stack.append(joint_to_pass_i);

      joint_to_sample_depth_stack.append(joint_to_sample_depth + 1);
      joint_to_sample_i_stack.append(joint_to_sample_i * 2 + 1);

      joint_to_pass_depht_stack.append(joint_to_pass_depth);
      joint_to_pass_i_stack.append(joint_to_pass_i);

      joint_to_sample_depth_stack.append(joint_to_sample_depth + 1);
      joint_to_sample_i_stack.append(joint_to_sample_i * 2 + 0);
      continue;
    }

    const int highest_depth_to_pass = rest_depth_to_pass[rest_depth_to_pass_i];

    const int highest_joint_i_to_pass = stack_to_i(highest_depth_to_pass, down_stack_to_pass);
    const IndexRange highest_buckets_to_pass = joint_buckets_range_at_depth(
        total_depth, highest_depth_to_pass, highest_joint_i_to_pass);
    const IndexRange highest_range_to_pass = buckets_offsets[highest_buckets_to_pass];
    BLI_assert(
        joint_predicate(joint_index_at_depth(highest_depth_to_pass, highest_joint_i_to_pass),
                        joint_to_sample_index));
    joint_func(highest_range_to_pass, joint_to_sample_index);
    // joint_func(joint_index_at_depth(highest_depth_to_pass, highest_joint_i_to_pass),
    // joint_to_sample_index);

    for (const int rest_depth_i :
         IndexRange::from_begin_end(joint_to_pass_depth, highest_depth_to_pass).drop_front(1))
    {
      const uint32_t down_stack_other_pass = stack_switch_branch(down_stack_to_pass, rest_depth_i);
      const int joint_i_other_pass = stack_to_i(rest_depth_i, down_stack_other_pass);
      if (joint_to_sample_depth == IndexRange(total_depth).last()) {
        joint_to_sample_depth_stack.append(joint_to_sample_depth);
        joint_to_sample_i_stack.append(joint_to_sample_i);

        joint_to_pass_depht_stack.append(rest_depth_i);
        joint_to_pass_i_stack.append(joint_i_other_pass);
      }
      else {
        joint_to_sample_depth_stack.append(joint_to_sample_depth + 1);
        joint_to_sample_i_stack.append(joint_to_sample_i * 2 + 1);

        joint_to_sample_depth_stack.append(joint_to_sample_depth + 1);
        joint_to_sample_i_stack.append(joint_to_sample_i * 2 + 0);

        joint_to_pass_depht_stack.append(rest_depth_i);
        joint_to_pass_i_stack.append(joint_i_other_pass);

        joint_to_pass_depht_stack.append(rest_depth_i);
        joint_to_pass_i_stack.append(joint_i_other_pass);
      }
    }
  }
}

static void sample_average_latest(const OffsetIndices<int> buckets_offsets,
                                  const int total_depth,
                                  const int power_value,
                                  const Span<float3> joints_centre,
                                  const Span<float> joints_radius,
                                  const Span<float> joints_min_distance,
                                  const Span<float> joints_value_factor,
                                  const Span<float3> joints_value,
                                  const Span<float3> bucket_position,
                                  const Span<float3> src_bucket_value,
                                  MutableSpan<float3> dst_buckets_data)
{
  BLI_assert(joints_centre.size() == joints_radius.size());
  BLI_assert(joints_centre.size() == joints_min_distance.size());
  BLI_assert(joints_centre.size() == joints_value_factor.size());
  BLI_assert(joints_centre.size() == joints_value.size());

  BLI_assert(bucket_position.size() == src_bucket_value.size());
  BLI_assert(bucket_position.size() == dst_buckets_data.size());

  const FunctionRef<void(int, MutableSpan<float>)> squared_distance_invertion =
      powered_rcp_for_squared(power_value);

  const auto accumulate_under = [&](const int parent_depth_i,
                                    const int parent_i,
                                    Vector<float> &distance_buffer) {
    for_each_to_bottom_latest(
        buckets_offsets,
        total_depth,
        parent_depth_i,
        parent_i,
        [&](const int joint_a, const int joint_b) -> bool {
          const float dist = math::distance_squared(joints_centre[joint_a],
                                                    joints_centre[joint_b]);
          const float min_dist = math::square(joints_radius[joint_a] +
                                              joints_min_distance[joint_b]);
          return dist > min_dist;
        },
        [&](const IndexRange range_to_pass, const int joint_to_sample) {
          const float3 sample_position = joints_centre[joint_to_sample];
          const float3 sample_value = joints_value[joint_to_sample];
          const float sample_factor = joints_value_factor[joint_to_sample];

          const Span<float3> self_position = bucket_position.slice(range_to_pass);
          const Span<float3> self_values = src_bucket_value.slice(range_to_pass);
          MutableSpan<float3> dst_values = dst_buckets_data.slice(range_to_pass);

          distance_buffer.resize(range_to_pass.size());
          squared_distance(sample_position, self_position, distance_buffer.as_mutable_span());
          squared_distance_invertion(power_value, distance_buffer.as_mutable_span());
          for (const int i : range_to_pass.index_range()) {
            dst_values[i] += (sample_value - self_values[i]) * distance_buffer[i] * sample_factor;
          }
        },
        [&](const IndexRange range_to_pass, const IndexRange range_to_sample) {
          BLI_assert(range_to_pass.intersect(range_to_sample).is_empty());
          const Span<float3> sample_position = bucket_position.slice(range_to_sample);
          const Span<float3> sample_value = src_bucket_value.slice(range_to_sample);

          const Span<float3> self_positions = bucket_position.slice(range_to_pass);
          const Span<float3> self_values = src_bucket_value.slice(range_to_pass);
          MutableSpan<float3> dst_values = dst_buckets_data.slice(range_to_pass);

          distance_buffer.resize(range_to_sample.size());
          for (const int index : range_to_pass.index_range()) {
            const float3 position = self_positions[index];
            const float3 value = self_values[index];

            squared_distance(position, sample_position, distance_buffer);
            squared_distance_invertion(power_value, distance_buffer.as_mutable_span());
            dst_values[index] += accumulate_difference(value, sample_value, distance_buffer);
          }
        });
  };

  for (const int depth_i : IndexRange(total_depth).drop_back(1)) {
    const IndexRange joints_range = joints_range_at_depth(depth_i);
    threading::parallel_for(
        joints_range.index_range(),
        1024 * 16,
        [&](const IndexRange range) {
          Vector<float> distance_buffer;
          for (const int joint_i : range) {
            accumulate_under(depth_i, joint_i, distance_buffer);
          }
        },
        threading::accumulated_task_sizes([&](const IndexRange joints_range) {
          return joint_size_at_depth(total_depth, depth_i) * joints_range.size();
        }));
  }
}

template<typename LeafFuncT, typename JointPredicateT, typename JointFuncT>
static void for_each_to_bottom_latest_linear(const OffsetIndices<int> buckets_offsets,
                                             const int total_depth,
                                             const int parent_depth_i,
                                             const int parent_i,
                                             const JointPredicateT &joint_predicate,
                                             const JointFuncT &joint_func,
                                             const LeafFuncT &leaf_func)
{
  BLI_assert(!buckets_offsets.is_empty());

  Vector<int, 64> joint_to_pass_depht_stack = {parent_depth_i + 1, parent_depth_i + 1};
  Vector<int, 64> joint_to_pass_i_stack = {parent_i * 2 + 1, parent_i * 2 + 0};
  Vector<int, 64> joint_to_sample_depth_stack = {parent_depth_i + 1, parent_depth_i + 1};
  Vector<int, 64> joint_to_sample_i_stack = {parent_i * 2 + 0, parent_i * 2 + 1};

  Vector<std::pair<int2, int>> range_to_joint;

  Vector<std::pair<int2, int2>> leafs_to_joint;

  while (!joint_to_pass_depht_stack.is_empty()) {
    BLI_assert(joint_to_pass_depht_stack.size() == joint_to_pass_i_stack.size());
    BLI_assert(joint_to_pass_depht_stack.size() == joint_to_sample_depth_stack.size());
    BLI_assert(joint_to_pass_depht_stack.size() == joint_to_sample_i_stack.size());

    const int joint_to_pass_depth = joint_to_pass_depht_stack.pop_last();
    const int joint_to_pass_i = joint_to_pass_i_stack.pop_last();
    const uint32_t stack_to_pass = joint_i_to_stack(joint_to_pass_depth, joint_to_pass_i);

    const int joint_to_sample_depth = joint_to_sample_depth_stack.pop_last();
    const int joint_to_sample_i = joint_to_sample_i_stack.pop_last();
    const int joint_to_sample_index = joint_index_at_depth(joint_to_sample_depth,
                                                           joint_to_sample_i);
    const uint32_t stack_to_sample = joint_i_to_stack(joint_to_sample_depth, joint_to_sample_i);

    BLI_assert(joint_to_pass_depth < total_depth);
    BLI_assert(joint_to_sample_depth < total_depth);

    const uint32_t to_pass_dirrection_stack = stack_from_highest_diff(stack_to_pass,
                                                                      stack_to_sample);
    const uint32_t down_stack_to_pass = merge_stacks(
        joint_to_pass_depth, stack_to_pass, to_pass_dirrection_stack);

    const IndexRange rest_depth_to_pass = IndexRange::from_begin_end(joint_to_pass_depth,
                                                                     total_depth);
    const int rest_depth_to_pass_i = binary_search::first_if(
        rest_depth_to_pass, [&](const int depth_i) {
          const int sub_joint_i_to_pass = stack_to_i(depth_i, down_stack_to_pass);
          const int sub_joint_index_to_pass = joint_index_at_depth(depth_i, sub_joint_i_to_pass);
          return joint_predicate(sub_joint_index_to_pass, joint_to_sample_index);
        });
    const bool need_to_double_sampler = rest_depth_to_pass_i == rest_depth_to_pass.size();

    if (need_to_double_sampler) {
      if (joint_to_sample_depth == IndexRange(total_depth).last()) {
        const IndexRange buckets_to_pass = joint_buckets_range_at_depth(
            total_depth, joint_to_pass_depth, joint_to_pass_i);
        const IndexRange range_to_pass = buckets_offsets[buckets_to_pass];

        const IndexRange buckets_to_sample = joint_buckets_range_at_depth(
            total_depth, joint_to_sample_depth, joint_to_sample_i);
        const IndexRange range_to_sample = buckets_offsets[buckets_to_sample];

        leafs_to_joint.append({int2(range_to_pass.start(), range_to_pass.size()),
                               int2(range_to_sample.start(), range_to_sample.size())});
        continue;
      }

      joint_to_pass_depht_stack.append(joint_to_pass_depth);
      joint_to_pass_i_stack.append(joint_to_pass_i);

      joint_to_sample_depth_stack.append(joint_to_sample_depth + 1);
      joint_to_sample_i_stack.append(joint_to_sample_i * 2 + 1);

      joint_to_pass_depht_stack.append(joint_to_pass_depth);
      joint_to_pass_i_stack.append(joint_to_pass_i);

      joint_to_sample_depth_stack.append(joint_to_sample_depth + 1);
      joint_to_sample_i_stack.append(joint_to_sample_i * 2 + 0);
      continue;
    }

    const int highest_depth_to_pass = rest_depth_to_pass[rest_depth_to_pass_i];

    const int highest_joint_i_to_pass = stack_to_i(highest_depth_to_pass, down_stack_to_pass);
    const IndexRange highest_buckets_to_pass = joint_buckets_range_at_depth(
        total_depth, highest_depth_to_pass, highest_joint_i_to_pass);
    const IndexRange highest_range_to_pass = buckets_offsets[highest_buckets_to_pass];
    BLI_assert(
        joint_predicate(joint_index_at_depth(highest_depth_to_pass, highest_joint_i_to_pass),
                        joint_to_sample_index));
    range_to_joint.append({int2(highest_range_to_pass.start(), highest_range_to_pass.size()),
                           joint_to_sample_index});

    for (const int rest_depth_i :
         IndexRange::from_begin_end(joint_to_pass_depth, highest_depth_to_pass).drop_front(1))
    {
      const uint32_t down_stack_other_pass = stack_switch_branch(down_stack_to_pass, rest_depth_i);
      const int joint_i_other_pass = stack_to_i(rest_depth_i, down_stack_other_pass);

      joint_to_sample_depth_stack.append(joint_to_sample_depth);
      joint_to_sample_i_stack.append(joint_to_sample_i);

      joint_to_pass_depht_stack.append(rest_depth_i);
      joint_to_pass_i_stack.append(joint_i_other_pass);
    }
  }

  threading::parallel_for(
      range_to_joint.index_range(), 1024'00000000, [&](const IndexRange range) {
        for (const std::pair<int2, int> to_pass : range_to_joint.as_span().slice(range)) {
          joint_func(IndexRange::from_begin_size(to_pass.first[0], to_pass.first[1]),
                     to_pass.second);
        }
      });

  threading::parallel_for(
      leafs_to_joint.index_range(), 1024'00000000, [&](const IndexRange range) {
        for (const std::pair<int2, int2> leaf : leafs_to_joint.as_span().slice(range)) {
          leaf_func(IndexRange::from_begin_size(leaf.first[0], leaf.first[1]),
                    IndexRange::from_begin_size(leaf.second[0], leaf.second[1]));
        }
      });
}

static void sample_average_latest_linear(const OffsetIndices<int> buckets_offsets,
                                         const int total_depth,
                                         const int power_value,
                                         const Span<float3> joints_centre,
                                         const Span<float> joints_radius,
                                         const Span<float> joints_min_distance,
                                         const Span<float> joints_value_factor,
                                         const Span<float3> joints_value,
                                         const Span<float3> bucket_position,
                                         const Span<float3> src_bucket_value,
                                         MutableSpan<float3> dst_buckets_data)
{
  BLI_assert(joints_centre.size() == joints_radius.size());
  BLI_assert(joints_centre.size() == joints_min_distance.size());
  BLI_assert(joints_centre.size() == joints_value_factor.size());
  BLI_assert(joints_centre.size() == joints_value.size());

  BLI_assert(bucket_position.size() == src_bucket_value.size());
  BLI_assert(bucket_position.size() == dst_buckets_data.size());

  const FunctionRef<void(int, MutableSpan<float>)> squared_distance_invertion =
      powered_rcp_for_squared(power_value);

  const auto accumulate_under = [&](const int parent_depth_i,
                                    const int parent_i,
                                    Vector<float> &distance_buffer) {
    for_each_to_bottom_latest_linear(
        buckets_offsets,
        total_depth,
        parent_depth_i,
        parent_i,
        [&](const int joint_a, const int joint_b) -> bool {
          const float dist = math::distance_squared(joints_centre[joint_a],
                                                    joints_centre[joint_b]);
          const float min_dist = math::square(joints_radius[joint_a] +
                                              joints_min_distance[joint_b]);
          return dist > min_dist;
        },
        [&](const IndexRange range_to_pass, const int joint_to_sample) {
          const float3 sample_position = joints_centre[joint_to_sample];
          const float3 sample_value = joints_value[joint_to_sample];
          const float sample_factor = joints_value_factor[joint_to_sample];

          const Span<float3> self_position = bucket_position.slice(range_to_pass);
          const Span<float3> self_values = src_bucket_value.slice(range_to_pass);
          MutableSpan<float3> dst_values = dst_buckets_data.slice(range_to_pass);

          Array<float> distance_buffer(range_to_pass.size());
          squared_distance(sample_position, self_position, distance_buffer.as_mutable_span());
          squared_distance_invertion(power_value, distance_buffer.as_mutable_span());
          for (const int i : range_to_pass.index_range()) {
            dst_values[i] += (sample_value - self_values[i]) * distance_buffer[i] * sample_factor;
          }
        },
        [&](const IndexRange range_to_pass, const IndexRange range_to_sample) {
          BLI_assert(range_to_pass.intersect(range_to_sample).is_empty());
          const Span<float3> sample_position = bucket_position.slice(range_to_sample);
          const Span<float3> sample_value = src_bucket_value.slice(range_to_sample);

          const Span<float3> self_positions = bucket_position.slice(range_to_pass);
          const Span<float3> self_values = src_bucket_value.slice(range_to_pass);
          MutableSpan<float3> dst_values = dst_buckets_data.slice(range_to_pass);

          Array<float> distance_buffer(range_to_sample.size());
          for (const int index : range_to_pass.index_range()) {
            const float3 position = self_positions[index];
            const float3 value = self_values[index];

            squared_distance(position, sample_position, distance_buffer);
            squared_distance_invertion(power_value, distance_buffer.as_mutable_span());
            dst_values[index] += accumulate_difference(value, sample_value, distance_buffer);
          }
        });
  };

  for (const int depth_i : IndexRange(total_depth).drop_back(1)) {
    const IndexRange joints_range = joints_range_at_depth(depth_i);
    threading::parallel_for(
        joints_range.index_range(),
        1024 * 16,
        [&](const IndexRange range) {
          Vector<float> distance_buffer;
          for (const int joint_i : range) {
            accumulate_under(depth_i, joint_i, distance_buffer);
          }
        },
        threading::accumulated_task_sizes([&](const IndexRange joints_range) {
          return joint_size_at_depth(total_depth, depth_i) * joints_range.size();
        }));
  }

  for_each_leaf(
      buckets_offsets,
      total_depth,
      GrainSize(4096),
      [&](const IndexRange bucket_range, const int /*joint_index*/, const int /*depth_i*/) {
        const Span<float3> self_positions = bucket_position.slice(bucket_range);
        const Span<float3> self_values = src_bucket_value.slice(bucket_range);
        MutableSpan<float3> dst_values = dst_buckets_data.slice(bucket_range);
        Array<float, min_bucket_size> distance_buffer(bucket_range.size());

        for (const int index : bucket_range.index_range()) {
          const float3 position = self_positions[index];
          const float3 value = self_values[index];

          squared_distance(position, self_positions, distance_buffer);
          squared_distance_invertion(power_value, distance_buffer.as_mutable_span());
          distance_buffer[index] = 0.0f;
          dst_values[index] += accumulate_difference(value, self_values, distance_buffer);
        }
      });
}

#endif

}  // namespace blender::akdbh
