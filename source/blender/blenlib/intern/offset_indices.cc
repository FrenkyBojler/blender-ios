/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <algorithm>

#include "BLI_array_utils.hh"
#include "BLI_offset_indices.hh"
#include "BLI_sort_radix.hh"
#include "BLI_task.hh"

namespace blender::offset_indices {

OffsetIndices<int> accumulate_counts_to_offsets(MutableSpan<int> counts_to_offsets,
                                                const int start_offset)
{
  int offset = start_offset;
  int64_t offset_i64 = start_offset;

  for (const int i : counts_to_offsets.index_range().drop_back(1)) {
    const int count = counts_to_offsets[i];
    BLI_assert(count >= 0);
    counts_to_offsets[i] = offset;
    offset += count;
#ifndef NDEBUG
    offset_i64 += count;
#endif
  }
  counts_to_offsets.last() = offset;

  BLI_assert_msg(offset == offset_i64, "Integer overflow occurred");
  UNUSED_VARS_NDEBUG(offset_i64);

  return OffsetIndices<int>(counts_to_offsets);
}

std::optional<OffsetIndices<int>> accumulate_counts_to_offsets_with_overflow_check(
    MutableSpan<int> counts_to_offsets, int start_offset)
{
  /* This variant was measured to be about ~8% slower than the version without overflow check.
   * Since this function is often a serial bottleneck, we use a separate code path for when an
   * overflow check is requested. */
  int64_t offset = start_offset;
  for (const int i : counts_to_offsets.index_range().drop_back(1)) {
    const int count = counts_to_offsets[i];
    BLI_assert(count >= 0);
    counts_to_offsets[i] = offset;
    offset += count;
  }
  counts_to_offsets.last() = offset;
  const bool has_overflow = offset >= std::numeric_limits<int>::max();
  if (has_overflow) {
    return std::nullopt;
  }
  return OffsetIndices<int>(counts_to_offsets);
}

void fill_constant_group_size(const int size, const int start_offset, MutableSpan<int> offsets)
{
  threading::memory_bandwidth_bound_task(offsets.size_in_bytes(), [&]() {
    threading::parallel_for(offsets.index_range(), 1024, [&](const IndexRange range) {
      for (const int64_t i : range) {
        offsets[i] = size * i + start_offset;
      }
    });
  });
}

void copy_group_sizes(const OffsetIndices<int> offsets,
                      const IndexMask &mask,
                      MutableSpan<int> sizes)
{
  mask.foreach_index_optimized<int64_t>([&](const int64_t i) { sizes[i] = offsets[i].size(); },
                                        exec_mode::grain_size(4096));
}

void gather_group_sizes(const OffsetIndices<int> offsets,
                        const IndexMask &mask,
                        MutableSpan<int> sizes)
{
  mask.foreach_index_optimized<int64_t>(
      [&](const int64_t i, const int64_t pos) { sizes[pos] = offsets[i].size(); },
      exec_mode::grain_size(4096));
}

void gather_group_sizes(const OffsetIndices<int> offsets,
                        const Span<int> indices,
                        MutableSpan<int> sizes)
{
  threading::memory_bandwidth_bound_task(
      sizes.size_in_bytes() + offsets.data().size_in_bytes() + indices.size_in_bytes(), [&]() {
        threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
          for (const int i : range) {
            sizes[i] = offsets[indices[i]].size();
          }
        });
      });
}

int sum_group_sizes(const OffsetIndices<int> offsets, const Span<int> indices)
{
  int count = 0;
  for (const int i : indices) {
    count += offsets[i].size();
  }
  return count;
}

int sum_group_sizes(const OffsetIndices<int> offsets, const IndexMask &mask)
{
  int count = 0;
  mask.foreach_segment_optimized([&](const auto segment) {
    if constexpr (std::is_same_v<std::decay_t<decltype(segment)>, IndexRange>) {
      count += offsets[segment].size();
    }
    else {
      for (const int64_t i : segment) {
        count += offsets[i].size();
      }
    }
  });
  return count;
}

OffsetIndices<int> gather_selected_offsets(const OffsetIndices<int> src_offsets,
                                           const IndexMask &selection,
                                           const int start_offset,
                                           MutableSpan<int> dst_offsets)
{
  if (selection.is_empty()) {
    return {};
  }
  int offset = start_offset;
  selection.foreach_index_optimized<int>([&](const int i, const int pos) {
    dst_offsets[pos] = offset;
    offset += src_offsets[i].size();
  });
  dst_offsets.last() = offset;
  return OffsetIndices<int>(dst_offsets);
}

void build_reverse_map(OffsetIndices<int> offsets, MutableSpan<int> r_map)
{
  threading::parallel_for(offsets.index_range(), 1024, [&](const IndexRange range) {
    for (const int64_t i : range) {
      r_map.slice(offsets[i]).fill(i);
    }
  });
}

OffsetIndices<int> build_reverse_offsets(const Span<int> indices, MutableSpan<int> offsets)
{
  BLI_assert(std::all_of(offsets.begin(), offsets.end(), [](int value) { return value == 0; }));
  array_utils::count_indices(indices, offsets);
  return offset_indices::accumulate_counts_to_offsets(offsets);
}

struct PositionValues {
  int operator()(const int64_t position) const
  {
    return int(position);
  }
};

struct RemapValues {
  Span<int> values;
  int operator()(const int64_t position) const
  {
    return values[position];
  }
};

void reverse_indices_in_groups(const Span<int> group_indices,
                               const OffsetIndices<int> offsets,
                               MutableSpan<int> results,
                               const Span<int> values)
{
  if (group_indices.is_empty()) {
    return;
  }
  BLI_assert(results.size() == group_indices.size());
  BLI_assert(results.size() == offsets.total_size());
  BLI_assert(values.is_empty() || values.size() == group_indices.size());
  BLI_assert(*std::max_element(group_indices.begin(), group_indices.end()) < offsets.size());
  BLI_assert(*std::min_element(group_indices.begin(), group_indices.end()) >= 0);

  const Span<int> known_offsets = offsets.data().drop_back(1);
  if (values.is_empty()) {
    radix_sort::sort_into_groups(
        group_indices, offsets.size(), known_offsets, PositionValues(), {}, results);
  }
  else {
    radix_sort::sort_into_groups(
        group_indices, offsets.size(), known_offsets, RemapValues{values}, {}, results);
  }
}

GroupedSpan<int> build_groups_from_indices(const Span<int> indices,
                                           const int groups_num,
                                           Array<int> &offset_data,
                                           Array<int> &index_data,
                                           const Span<int> values)
{
  BLI_assert(values.is_empty() || values.size() == indices.size());
  if (indices.is_empty()) {
    offset_data = Array<int>(groups_num + 1, 0);
    index_data.reinitialize(0);
    return {OffsetIndices<int>(offset_data), index_data};
  }

  offset_data = Array<int>(groups_num + 1);
  index_data.reinitialize(indices.size());
  if (values.is_empty()) {
    radix_sort::sort_into_groups(
        indices, groups_num, {}, PositionValues(), offset_data, index_data);
  }
  else {
    radix_sort::sort_into_groups(
        indices, groups_num, {}, RemapValues{values}, offset_data, index_data);
  }
  return {OffsetIndices<int>(offset_data), index_data};
}

}  // namespace blender::offset_indices
