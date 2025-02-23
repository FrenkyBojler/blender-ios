/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "BLI_map.hh"
#include "BLI_array.hh"
#include "BLI_task.hh"
#include "BLI_assert.h"

#include "GEO_abstract_kd_bucket_hierarchy.hh"

#include "testing/testing.h"

namespace blender::geometry::akdbh::tests {

static void test()
{
  BLI_assert(total_depth_from_total(100) == 4);

  BLI_assert(total_joints_at_start(0) == 0);
  BLI_assert(total_joints_at_start(1) == 1);
  BLI_assert(total_joints_at_start(2) == 3);
  BLI_assert(total_joints_at_start(3) == 7);

  BLI_assert(total_joints_at_depth(0) == 1);
  BLI_assert(total_joints_at_depth(1) == 2);
  BLI_assert(total_joints_at_depth(2) == 4);
  BLI_assert(total_joints_at_depth(3) == 8);

  BLI_assert(joint_size_at_depth(1, 0) == 1);

  BLI_assert(joint_size_at_depth(2, 0) == 2);
  BLI_assert(joint_size_at_depth(2, 1) == 1);

  BLI_assert(joint_size_at_depth(3, 0) == 4);
  BLI_assert(joint_size_at_depth(3, 1) == 2);
  BLI_assert(joint_size_at_depth(3, 2) == 1);

  BLI_assert(joints_range_at_depth(0) == IndexRange::from_begin_size(0, 1));
  BLI_assert(joints_range_at_depth(1) == IndexRange::from_begin_size(1, 2));
  BLI_assert(joints_range_at_depth(2) == IndexRange::from_begin_size(1 + 2, 4));
  BLI_assert(joints_range_at_depth(3) == IndexRange::from_begin_size(1 + 2 + 4, 8));

  BLI_assert(joint_buckets_range_at_depth(4, 0, 0) == IndexRange::from_begin_size(0, 8));

  BLI_assert(joint_buckets_range_at_depth(4, 1, 0) == IndexRange::from_begin_size(0, 4));
  BLI_assert(joint_buckets_range_at_depth(4, 1, 1) == IndexRange::from_begin_size(4, 4));

  BLI_assert(joint_buckets_range_at_depth(4, 2, 0) == IndexRange::from_begin_size(0, 2));
  BLI_assert(joint_buckets_range_at_depth(4, 2, 1) == IndexRange::from_begin_size(2, 2));
  BLI_assert(joint_buckets_range_at_depth(4, 2, 2) == IndexRange::from_begin_size(4, 2));
  BLI_assert(joint_buckets_range_at_depth(4, 2, 3) == IndexRange::from_begin_size(6, 2));

  BLI_assert(joint_buckets_range_at_depth(4, 3, 0) == IndexRange::from_begin_size(0, 1));
  BLI_assert(joint_buckets_range_at_depth(4, 3, 1) == IndexRange::from_begin_size(1, 1));
  BLI_assert(joint_buckets_range_at_depth(4, 3, 2) == IndexRange::from_begin_size(2, 1));
  BLI_assert(joint_buckets_range_at_depth(4, 3, 3) == IndexRange::from_begin_size(3, 1));
  BLI_assert(joint_buckets_range_at_depth(4, 3, 4) == IndexRange::from_begin_size(4, 1));
  BLI_assert(joint_buckets_range_at_depth(4, 3, 5) == IndexRange::from_begin_size(5, 1));
  BLI_assert(joint_buckets_range_at_depth(4, 3, 6) == IndexRange::from_begin_size(6, 1));
  BLI_assert(joint_buckets_range_at_depth(4, 3, 7) == IndexRange::from_begin_size(7, 1));
}

static void test2()
{
  const int total_elements = 100;
  const int total_depth = total_depth_from_total(total_elements);
  const int total_buckets = total_buckets_for(total_depth);
  const int total_joints = total_joints_for_depth(total_depth);

  BLI_assert(total_depth == 4);
  BLI_assert(total_buckets == 8);
  BLI_assert(total_joints == (8 + 4 + 2 + 1));

  Array<int> bucket_indices(total_buckets + 1);
  const OffsetIndices<int> buckets_offsets = fill_bucket_offsets_trivial(total_elements, bucket_indices);

  const Array<int> test_indices({0, 12, 25, 37, 50, 62, 75, 87, 100});
  BLI_assert(bucket_indices.as_span() == test_indices.as_span());

  Map<int, IndexRange> leaf_joint_to_bucket;
  for_each_leaf(buckets_offsets,
                total_depth,
                GrainSize(1'000'000'000),
                [&](const IndexRange bucket_range, const int joint_index, const int depth_i) {
                  BLI_assert(total_depth - 1 == depth_i);
                  BLI_assert(leaf_joint_to_bucket.add(joint_index, bucket_range));
                });

  Map<int, IndexRange> test_leaf_joint_to_bucket;
  test_leaf_joint_to_bucket.add(7 + 0, buckets_offsets[0]);
  test_leaf_joint_to_bucket.add(7 + 1, buckets_offsets[1]);
  test_leaf_joint_to_bucket.add(7 + 2, buckets_offsets[2]);
  test_leaf_joint_to_bucket.add(7 + 3, buckets_offsets[3]);
  test_leaf_joint_to_bucket.add(7 + 4, buckets_offsets[4]);
  test_leaf_joint_to_bucket.add(7 + 5, buckets_offsets[5]);
  test_leaf_joint_to_bucket.add(7 + 6, buckets_offsets[6]);
  test_leaf_joint_to_bucket.add(7 + 7, buckets_offsets[7]);

  BLI_assert(test_leaf_joint_to_bucket == leaf_joint_to_bucket);

  Map<std::pair<int, int>, IndexRange> joints_to_bottom;
  for_each_to_bottom(
      buckets_offsets,
      total_depth,
      GrainSize(1'000'000'000),
      [&](const IndexRange bucket_range, int joint_index, int depth_i) {
        BLI_assert(joints_to_bottom.add(std::pair<int, int>(joint_index, depth_i), bucket_range));
      });

  Map<std::pair<int, int>, IndexRange> test_joints_to_bottom;
  test_joints_to_bottom.add(std::pair<int, int>(0, 0), buckets_offsets[IndexRange(0, 8)]);

  test_joints_to_bottom.add(std::pair<int, int>(1, 1), buckets_offsets[IndexRange(0, 4)]);
  test_joints_to_bottom.add(std::pair<int, int>(2, 1), buckets_offsets[IndexRange(4, 4)]);

  test_joints_to_bottom.add(std::pair<int, int>(3, 2), buckets_offsets[IndexRange(0, 2)]);
  test_joints_to_bottom.add(std::pair<int, int>(4, 2), buckets_offsets[IndexRange(2, 2)]);
  test_joints_to_bottom.add(std::pair<int, int>(5, 2), buckets_offsets[IndexRange(4, 2)]);
  test_joints_to_bottom.add(std::pair<int, int>(6, 2), buckets_offsets[IndexRange(6, 2)]);

  BLI_assert(joints_to_bottom == test_joints_to_bottom);

  Map<std::pair<int, int>, std::pair<int2, IndexRange>> joints_to_top;
  for_each_to_top(
      buckets_offsets,
      total_depth,
      GrainSize(1'000'000'000),
      [&](const IndexRange bucket_range, int joint_index, const int2 sub_joints, int depth_i) {
        BLI_assert(joints_to_top.add(std::pair<int, int>(joint_index, depth_i),
                                     std::pair<int2, IndexRange>(sub_joints, bucket_range)));
      });

  Map<std::pair<int, int>, std::pair<int2, IndexRange>> test_joints_to_top;
  test_joints_to_top.add(
      std::pair<int, int>(3, 2),
      std::pair<int2, IndexRange>(int2(7, 8), buckets_offsets[IndexRange(0, 2)]));
  test_joints_to_top.add(
      std::pair<int, int>(4, 2),
      std::pair<int2, IndexRange>(int2(9, 10), buckets_offsets[IndexRange(2, 2)]));
  test_joints_to_top.add(
      std::pair<int, int>(5, 2),
      std::pair<int2, IndexRange>(int2(11, 12), buckets_offsets[IndexRange(4, 2)]));
  test_joints_to_top.add(
      std::pair<int, int>(6, 2),
      std::pair<int2, IndexRange>(int2(13, 14), buckets_offsets[IndexRange(6, 2)]));

  test_joints_to_top.add(
      std::pair<int, int>(1, 1),
      std::pair<int2, IndexRange>(int2(3, 4), buckets_offsets[IndexRange(0, 4)]));
  test_joints_to_top.add(
      std::pair<int, int>(2, 1),
      std::pair<int2, IndexRange>(int2(5, 6), buckets_offsets[IndexRange(4, 4)]));

  test_joints_to_top.add(
      std::pair<int, int>(0, 0),
      std::pair<int2, IndexRange>(int2(1, 2), buckets_offsets[IndexRange(0, 8)]));

  BLI_assert(joints_to_top == test_joints_to_top);
}

static void test3()
{
  BLI_assert(joint_i_to_stack(1, 0) == 0b00000000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(1, 1) == 0b10000000'00000000'00000000'00000000);

  BLI_assert(joint_i_to_stack(2, 0) == 0b00000000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(2, 1) == 0b01000000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(2, 2) == 0b10000000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(2, 3) == 0b11000000'00000000'00000000'00000000);

  BLI_assert(joint_i_to_stack(3, 0) == 0b00000000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(3, 1) == 0b00100000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(3, 2) == 0b01000000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(3, 3) == 0b01100000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(3, 4) == 0b10000000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(3, 5) == 0b10100000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(3, 6) == 0b11000000'00000000'00000000'00000000);
  BLI_assert(joint_i_to_stack(3, 7) == 0b11100000'00000000'00000000'00000000);

  BLI_assert(stack_mask(1) == 0b10000000'00000000'00000000'00000000);
  BLI_assert(stack_mask(2) == 0b11000000'00000000'00000000'00000000);
  BLI_assert(stack_mask(3) == 0b11100000'00000000'00000000'00000000);

  BLI_assert(stack_to_i(1, joint_i_to_stack(1, 0)) == 0);
  BLI_assert(stack_to_i(1, joint_i_to_stack(1, 1)) == 1);

  BLI_assert(stack_to_i(2, joint_i_to_stack(2, 0)) == 0);
  BLI_assert(stack_to_i(2, joint_i_to_stack(2, 1)) == 1);
  BLI_assert(stack_to_i(2, joint_i_to_stack(2, 2)) == 2);
  BLI_assert(stack_to_i(2, joint_i_to_stack(2, 3)) == 3);

  BLI_assert(stack_to_i(3, joint_i_to_stack(3, 0)) == 0);
  BLI_assert(stack_to_i(3, joint_i_to_stack(3, 1)) == 1);
  BLI_assert(stack_to_i(3, joint_i_to_stack(3, 2)) == 2);
  BLI_assert(stack_to_i(3, joint_i_to_stack(3, 3)) == 3);
  BLI_assert(stack_to_i(3, joint_i_to_stack(3, 4)) == 4);
  BLI_assert(stack_to_i(3, joint_i_to_stack(3, 5)) == 5);
  BLI_assert(stack_to_i(3, joint_i_to_stack(3, 6)) == 6);
  BLI_assert(stack_to_i(3, joint_i_to_stack(3, 7)) == 7);

  BLI_assert(stack_switch_branch(0b00000000'00000000'00000000'00000000, 1) ==
             0b10000000'00000000'00000000'00000000);
  BLI_assert(stack_switch_branch(0b00100000'00000000'00000000'00000000, 1) ==
             0b10100000'00000000'00000000'00000000);
  BLI_assert(stack_switch_branch(0b01000000'00000000'00000000'00000000, 1) ==
             0b11000000'00000000'00000000'00000000);
  BLI_assert(stack_switch_branch(0b01100000'00000000'00000000'00000000, 1) ==
             0b11100000'00000000'00000000'00000000);
  BLI_assert(stack_switch_branch(0b10000000'00000000'00000000'00000000, 2) ==
             0b11000000'00000000'00000000'00000000);
  BLI_assert(stack_switch_branch(0b10100000'00000000'00000000'00000000, 2) ==
             0b11100000'00000000'00000000'00000000);
  BLI_assert(stack_switch_branch(0b11000000'00000000'00000000'00000000, 2) ==
             0b10000000'00000000'00000000'00000000);
  BLI_assert(stack_switch_branch(0b11100000'00000000'00000000'00000000, 3) ==
             0b11000000'00000000'00000000'00000000);
}

/*
static void test5(const OffsetIndices<int> base_offsets,
                                   const int total_depth,
                                   const Span<float> joints_min_radii,
                                   const Span<float> joints_min_distance,
                                   const Span<float3> joints_min_distance)
{
  Array<int64_t> src_bucket_values(base_offsets.total_size());

  RandomNumberGenerator generator;
  std::generate(
      src_bucket_values.begin(), src_bucket_values.end(), [&] { return generator.get_int32(); });

  const int64_t total_value = std::accumulate(
      src_bucket_values.begin(), src_bucket_values.end(), 0);

  Array<int64_t> joint_value(joints_min_radii.size());
  mean_sums<int64_t>(const OffsetIndices<int> base_offsets,
                     const int total_depth,
                     src_bucket_values,
                     joint_value);

  Array<int64_t> dst_bucket_values(base_offsets.total_size(), 0);
  for_each_to_bottom_latest(
      base_offsets,
      total_depth,
      [&](const int joint_a, const int joint_b) -> bool {
        BLI_assert(joints_min_radii[joint_a] >= 0.0f);
        BLI_assert(joints_min_distance[joint_b] >= 0.0f);
        const float dist = math::distance(joints_positions[joint_a], joints_positions[joint_b]);
        const float min_dist = joints_min_radii[joint_a] + joints_min_distance[joint_b];
        return dist > min_dist;
      },
      [&](const IndexRange range_to_pass, const int joint_to_sample) {
        // [&](const int joint_to_pass, const int joint_to_sample) {
        for (int64_t &value : dst_bucket_values.as_mutable_span().slice(range_to_pass)) {
          value += joint_value[joint_to_sample];
        }
      },
      [&](const IndexRange range_to_pass, const IndexRange range_to_sample) {
        // [&](const int bucket_to_pass, const int bucket_to_sample) {
        for (int64_t &value : dst_bucket_values.as_mutable_span().slice(range_to_pass)) {
          for (const int64_t other : src_bucket_values.as_span().slice(range_to_sample)) {
            value += other;
          }
        }
      });

  akdbt::for_each_leaf(
      base_offsets,
      total_depth,
      GrainSize(10'000'000'000),
      [&](const IndexRange bucket_range, const int joint_index, const int depth_i) {});

  for (const int value_i : IndexRange(base_offsets.total_size())) {
    BLI_assert(dst_bucket_values[value_i] == total_value - src_bucket_values[value_i]);
  }
}
*/

}  // namespace blender::geometry::akdbh::tests
