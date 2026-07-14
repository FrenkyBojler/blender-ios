/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_index_mask.hh"
#include "BLI_offset_indices.hh"
#include "BLI_vector.hh"

#include "BLI_strict_flags.hh" /* IWYU pragma: keep. Keep last. */

namespace blender::offset_indices::tests {

TEST(offset_indices, SumSizes)
{
  Vector<int> data = {3, 2, 1, 5, -1};
  const OffsetIndices<int> offsets = accumulate_counts_to_offsets(data);
  EXPECT_EQ(sum_group_sizes(offsets, {0, 1, 2, 3}), 11);
  EXPECT_EQ(sum_group_sizes(offsets, {3, 2, 1, 0}), 11);
  EXPECT_EQ(sum_group_sizes(offsets, {3, 0}), 8);
  EXPECT_EQ(sum_group_sizes(offsets, IndexRange(4)), 11);
  EXPECT_EQ(sum_group_sizes(offsets, IndexMask(4)), 11);
  EXPECT_EQ(sum_group_sizes(offsets, IndexMask(1)), 3);
}

TEST(offset_indices, build_groups_from_indices)
{
  Vector<int> data = {3, 2, 1, 3, 4, 1, 1, 6, 8, 1, 8, 0};
  const int groups_num = 10;

  Array<int> offset_data;
  Array<int> index_data;
  const GroupedSpan<int> groups = build_groups_from_indices(
      data, groups_num, offset_data, index_data);

  EXPECT_EQ(groups.size(), groups_num);
  EXPECT_EQ(groups.offsets.total_size(), data.size());
  EXPECT_EQ_SPAN(groups[1], {2, 5, 6, 9});
  EXPECT_TRUE(groups[5].is_empty());
}

TEST(offset_indices, build_groups_from_indices_empty)
{
  Array<int> offset_data;
  Array<int> index_data;
  const GroupedSpan<int> groups = build_groups_from_indices({}, 4, offset_data, index_data);
  EXPECT_EQ(groups.size(), 4);
  EXPECT_EQ(groups.offsets.total_size(), 0);

  const GroupedSpan<int> no_groups = build_groups_from_indices({}, 0, offset_data, index_data);
  EXPECT_EQ(no_groups.size(), 0);
}

TEST(offset_indices, build_groups_from_indices_few_groups)
{
  const int groups_num = 5;
  const int64_t indices_num = 200000;
  Array<int> data(indices_num);
  for (const int64_t i : data.index_range()) {
    data[i] = int((i * i) % groups_num);
  }

  Array<int> offset_data;
  Array<int> index_data;
  const GroupedSpan<int> groups = build_groups_from_indices(
      data, groups_num, offset_data, index_data);

  EXPECT_EQ(groups.size(), groups_num);
  EXPECT_EQ(groups.offsets.total_size(), indices_num);
  for (const int64_t group : IndexRange(groups_num)) {
    int64_t previous = -1;
    for (const int index : groups[group]) {
      EXPECT_EQ(data[index], group);
      EXPECT_GT(index, previous);
      previous = index;
    }
  }
}

TEST(offset_indices, build_groups_from_indices_many_groups)
{
  const int groups_num = 100000;
  const int64_t indices_num = 300000;
  Array<int> data(indices_num);
  Array<int> group_sizes(groups_num, 0);
  for (const int64_t i : data.index_range()) {
    data[i] = (i % 4 != 0) ? 12345 : int((i * 7919) % (groups_num / 2));
    group_sizes[data[i]]++;
  }

  Array<int> offset_data;
  Array<int> index_data;
  const GroupedSpan<int> groups = build_groups_from_indices(
      data, groups_num, offset_data, index_data);

  EXPECT_EQ(groups.size(), groups_num);
  EXPECT_EQ(groups.offsets.total_size(), indices_num);
  for (const int64_t group : IndexRange(groups_num)) {
    EXPECT_EQ(groups[group].size(), group_sizes[int(group)]);
    int64_t previous = -1;
    for (const int index : groups[group]) {
      EXPECT_EQ(data[index], group);
      EXPECT_GT(index, previous);
      previous = index;
    }
  }
}

TEST(offset_indices, reverse_indices_in_groups_few_groups)
{
  const int groups_num = 7;
  const int64_t indices_num = 150000;
  Array<int> data(indices_num);
  Array<int> offset_data(groups_num + 1, 0);
  for (const int64_t i : data.index_range()) {
    data[i] = int((i * 13) % groups_num);
    offset_data[data[i]]++;
  }
  const OffsetIndices<int> offsets = accumulate_counts_to_offsets(offset_data);

  Array<int> results(indices_num);
  reverse_indices_in_groups(data, offsets, results);

  for (const int64_t group : IndexRange(groups_num)) {
    int64_t previous = -1;
    for (const int index : results.as_span().slice(offsets[group])) {
      EXPECT_EQ(data[index], group);
      EXPECT_GT(index, previous);
      previous = index;
    }
  }
}

TEST(offset_indices, reverse_indices_in_groups_many_groups)
{
  const int groups_num = 20000;
  const int64_t indices_num = 200000;
  Array<int> data(indices_num);
  Array<int> offset_data(groups_num + 1, 0);
  for (const int64_t i : data.index_range()) {
    data[i] = int((i * 4241) % groups_num);
    offset_data[data[i]]++;
  }
  const OffsetIndices<int> offsets = accumulate_counts_to_offsets(offset_data);

  Array<int> results(indices_num);
  reverse_indices_in_groups(data, offsets, results);

  for (const int64_t group : IndexRange(groups_num)) {
    int64_t previous = -1;
    for (const int index : results.as_span().slice(offsets[group])) {
      EXPECT_EQ(data[index], group);
      EXPECT_GT(index, previous);
      previous = index;
    }
  }
}

}  // namespace blender::offset_indices::tests
