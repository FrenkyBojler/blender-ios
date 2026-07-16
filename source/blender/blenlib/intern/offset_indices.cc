/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 */

#include <algorithm>

#include "BLI_array_utils.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_sort.hh"
#include "BLI_task.hh"
#include "BLI_task_size_hints.hh"

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

void sort_groups(const OffsetIndices<int> groups, MutableSpan<int> indices)
{
  threading::parallel_for(
      groups.index_range(),
      1024,
      [&](const IndexRange range) {
        for (const int64_t index : range) {
          MutableSpan<int> group = indices.slice(groups[index]);
          parallel_sort(group);
        }
      },
      threading::accumulated_task_sizes(
          [&](const IndexRange range) { return groups[range].size(); }));
}

/* -------------------------------------------------------------------- */
/** \name Counting and Radix Sort
 *
 * Utilities to implement efficient sorting for #reverse_indices_in_groups
 * and #build_groups_from_indices. This is better than a naive implementation
 * using atomics, where writes become heavily contended with few groups and
 * many CPU cores.
 *
 * In the simple case with few groups this uses a counting sort. The indices
 * array is handled in chunks in parallel, each counting the number of members
 * of every group. This is followed by a serial prefix sum and parallel
 * filling of the indices.
 *
 * When there are many groups, a radix sort is used. Indices are first split
 * into buckets of consecutive groups, and then a second pass sorts every
 * bucket individually.
 *
 * \{ */

/** Number of indices that are counted as one chunk. */
static constexpr int64_t COUNTING_SORT_CHUNK_SIZE = 32768;

/** Maximum number of groups sorted with a single pass counting sort. */
static constexpr int64_t COUNTING_SORT_MAX_GROUPS = 16384;

/** Maximum number of buckets sorted in the first pass of the radix sort. A small number keeps
 * the first pass cache friendly, at the cost of more groups per bucket in the second pass. */
static constexpr int64_t RADIX_SORT_MAX_BUCKETS = 512;

/**
 * Compute shift so that `group >> shift` maps a group to a bucket. In simple
 * case this returns 0 and the group index is the bucket index.
 */
static int radix_sort_bucket_shift(const int64_t groups_num)
{
  int shift = 0;
  while (((groups_num - 1) >> shift) >= RADIX_SORT_MAX_BUCKETS) {
    shift++;
  }
  return shift;
}

/** Parallel count the number of occurrences of every bucket in a chunk of indices. */
static void count_indices_per_chunk(const Span<int> indices,
                                    const int shift,
                                    const int64_t buckets_num,
                                    MutableSpan<int> chunk_counts)
{
  const int64_t chunks_num = chunk_counts.size() / buckets_num;
  threading::parallel_for(IndexRange(chunks_num), 1, [&](const IndexRange range) {
    for (const int64_t chunk : range) {
      MutableSpan<int> counts = chunk_counts.slice(chunk * buckets_num, buckets_num);
      counts.fill(0);
      const int64_t begin = chunk * COUNTING_SORT_CHUNK_SIZE;
      for (const int index : indices.slice_safe(begin, COUNTING_SORT_CHUNK_SIZE)) {
        counts[index >> shift]++;
      }
    }
  });
}

/** Serial prefix sum converting per-chunk bucket counts into offsets within the bucket. */
static void chunk_counts_to_offsets(MutableSpan<int> chunk_counts, MutableSpan<int> bucket_counts)
{
  const int64_t buckets_num = bucket_counts.size();
  const int64_t chunks_num = chunk_counts.size() / buckets_num;
  for (const int64_t chunk : IndexRange(chunks_num)) {
    MutableSpan<int> counts = chunk_counts.slice(chunk * buckets_num, buckets_num);
    for (const int64_t bucket : IndexRange(buckets_num)) {
      const int count = counts[bucket];
      counts[bucket] = bucket_counts[bucket];
      bucket_counts[bucket] += count;
    }
  }
}

/** Parallel fill the index of every element into the range of its group. */
static void fill_group_indices_per_chunk(const Span<int> indices,
                                         const Span<int> group_offsets,
                                         MutableSpan<int> chunk_offsets,
                                         MutableSpan<int> r_indices)
{
  const int64_t groups_num = group_offsets.size();
  const int64_t chunks_num = chunk_offsets.size() / groups_num;
  threading::parallel_for(IndexRange(chunks_num), 1, [&](const IndexRange range) {
    for (const int64_t chunk : range) {
      MutableSpan<int> current_offsets = chunk_offsets.slice(chunk * groups_num, groups_num);
      const int64_t begin = chunk * COUNTING_SORT_CHUNK_SIZE;
      const Span<int> chunk_indices = indices.slice_safe(begin, COUNTING_SORT_CHUNK_SIZE);
      for (const int64_t i : chunk_indices.index_range()) {
        const int group = chunk_indices[i];
        r_indices[group_offsets[group] + current_offsets[group]++] = int(begin + i);
      }
    }
  });
}

/**
 * Parallel fill the index of every element into the range of its bucket
 * as (index, group). To prepare for sorting buckets in a second pass.
 */
static void fill_bucket_indices_per_chunk(const Span<int> indices,
                                          const int shift,
                                          const Span<int> bucket_offsets,
                                          MutableSpan<int> chunk_offsets,
                                          MutableSpan<int2> r_pairs)
{
  const int64_t buckets_num = bucket_offsets.size();
  const int64_t chunks_num = chunk_offsets.size() / buckets_num;
  threading::parallel_for(IndexRange(chunks_num), 1, [&](const IndexRange range) {
    for (const int64_t chunk : range) {
      MutableSpan<int> current_offsets = chunk_offsets.slice(chunk * buckets_num, buckets_num);
      const int64_t begin = chunk * COUNTING_SORT_CHUNK_SIZE;
      const Span<int> chunk_indices = indices.slice_safe(begin, COUNTING_SORT_CHUNK_SIZE);
      for (const int64_t i : chunk_indices.index_range()) {
        const int group = chunk_indices[i];
        const int bucket = group >> shift;
        r_pairs[bucket_offsets[bucket] + current_offsets[bucket]++] = int2(int(begin + i), group);
      }
    }
  });
}

/**
 * Sort the index of every element into the range of its group, using either
 * known group offsets or computing them.
 */
static void sort_indices_into_groups(const Span<int> indices,
                                     const int64_t groups_num,
                                     const Span<int> known_group_offsets,
                                     MutableSpan<int> r_offset_data,
                                     MutableSpan<int> r_indices)
{
  BLI_assert(!indices.is_empty());
  const bool compute_offsets = known_group_offsets.is_empty();
  BLI_assert(compute_offsets ? r_offset_data.size() == groups_num + 1 :
                               known_group_offsets.size() == groups_num);

  const int shift = radix_sort_bucket_shift(groups_num);
  const int64_t chunks_num = divide_ceil_ul(indices.size(), COUNTING_SORT_CHUNK_SIZE);

  if (groups_num <= COUNTING_SORT_MAX_GROUPS || shift == 0) {
    /* Few groups, single pass counting sort. */
    Array<int> chunk_counts(chunks_num * groups_num);
    count_indices_per_chunk(indices, 0, groups_num, chunk_counts);

    Span<int> group_offsets = known_group_offsets;
    Array<int> group_counts;
    if (compute_offsets) {
      r_offset_data.fill(0);
      chunk_counts_to_offsets(chunk_counts, r_offset_data.drop_back(1));
      accumulate_counts_to_offsets(r_offset_data);
      group_offsets = r_offset_data.drop_back(1);
    }
    else {
      group_counts = Array<int>(groups_num, 0);
      chunk_counts_to_offsets(chunk_counts, group_counts);
    }

    fill_group_indices_per_chunk(indices, group_offsets, chunk_counts, r_indices);
    return;
  }

  /* Many groups, two pass radix sort. */
  const int64_t buckets_num = ((groups_num - 1) >> shift) + 1;
  Array<int> chunk_counts(chunks_num * buckets_num);
  count_indices_per_chunk(indices, shift, buckets_num, chunk_counts);

  Array<int> bucket_offset_data(buckets_num + 1, 0);
  if (compute_offsets) {
    chunk_counts_to_offsets(chunk_counts, bucket_offset_data.as_mutable_span().drop_back(1));
    accumulate_counts_to_offsets(bucket_offset_data);
  }
  else {
    for (const int64_t bucket : IndexRange(buckets_num)) {
      bucket_offset_data[bucket] = known_group_offsets[bucket << shift];
    }
    bucket_offset_data[buckets_num] = int(indices.size());
    Array<int> bucket_counts(buckets_num, 0);
    chunk_counts_to_offsets(chunk_counts, bucket_counts);
  }
  const OffsetIndices<int> bucket_offsets(bucket_offset_data.as_span());

  Array<int2> pairs(indices.size());
  fill_bucket_indices_per_chunk(
      indices, shift, bucket_offset_data.as_span().drop_back(1), chunk_counts, pairs);

  /* Sort every bucket by group. */
  const int64_t groups_per_bucket = int64_t(1) << shift;
  threading::EnumerableThreadSpecific<Array<int>> all_current_offsets(
      [&]() { return Array<int>(groups_per_bucket); });
  threading::parallel_for(
      IndexRange(buckets_num),
      1,
      [&](const IndexRange range) {
        MutableSpan<int> buffer = all_current_offsets.local();
        for (const int64_t bucket : range) {
          const int64_t group_begin = bucket << shift;
          const int64_t bucket_groups_num = std::min(groups_per_bucket, groups_num - group_begin);
          MutableSpan<int> current_offsets = buffer.take_front(bucket_groups_num);
          const IndexRange bucket_range = bucket_offsets[bucket];
          const Span<int2> bucket_pairs = pairs.as_span().slice(bucket_range);

          if (compute_offsets) {
            current_offsets.fill(0);
            for (const int2 &pair : bucket_pairs) {
              current_offsets[pair.y - group_begin]++;
            }
            int offset = int(bucket_range.start());
            for (const int64_t group : IndexRange(bucket_groups_num)) {
              const int count = current_offsets[group];
              r_offset_data[group_begin + group] = offset;
              current_offsets[group] = offset;
              offset += count;
            }
          }
          else {
            current_offsets.copy_from(known_group_offsets.slice(group_begin, bucket_groups_num));
          }

          for (const int2 &pair : bucket_pairs) {
            r_indices[current_offsets[pair.y - group_begin]++] = pair.x;
          }
        }
      },
      threading::accumulated_task_sizes(
          [&](const IndexRange range) { return bucket_offsets[range].size(); }));

  if (compute_offsets) {
    r_offset_data.last() = int(indices.size());
  }
}

/** \} */

void reverse_indices_in_groups(const Span<int> group_indices,
                               const OffsetIndices<int> offsets,
                               MutableSpan<int> results)
{
  if (group_indices.is_empty()) {
    return;
  }
  BLI_assert(results.size() == group_indices.size());
  BLI_assert(results.size() == offsets.total_size());
  BLI_assert(*std::max_element(group_indices.begin(), group_indices.end()) < offsets.size());
  BLI_assert(*std::min_element(group_indices.begin(), group_indices.end()) >= 0);

  sort_indices_into_groups(
      group_indices, offsets.size(), offsets.data().drop_back(1), {}, results);
}

GroupedSpan<int> build_groups_from_indices(const Span<int> indices,
                                           const int groups_num,
                                           Array<int> &offset_data,
                                           Array<int> &index_data)
{
  if (indices.is_empty()) {
    offset_data = Array<int>(groups_num + 1, 0);
    index_data.reinitialize(0);
    return {OffsetIndices<int>(offset_data), index_data};
  }

  offset_data = Array<int>(groups_num + 1);
  index_data.reinitialize(indices.size());
  sort_indices_into_groups(indices, groups_num, {}, offset_data, index_data);
  return {OffsetIndices<int>(offset_data), index_data};
}

}  // namespace blender::offset_indices
