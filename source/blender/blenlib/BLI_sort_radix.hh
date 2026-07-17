/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Building blocks for parallel radix sorting and partitioning. Designed for efficient
 * sorting of integer keys, that correspond to a number of groups that is approximately
 * equal to or smaller than the number of keys to be sorted.
 *
 * A naive solution to this problem is a counting sort with atomics. However that does
 * not scale to many cores, heavy lock contention can make it slower than serial.
 *
 * Instead there are 3 algorithms with increasing complexity and overhead.
 *
 * - For a small number of keys, a serial counting sort is most efficient.
 * - For a small number of groups, a parallel counting sort works best. The keys
 *   are split into chunks, with counting performed for each chunk in parallel. This
 *   is followed by serial summing of per-chunk counts and offsets, and parallel
 *   filling per chunk.
 * - For a large number of groups, radix sort is used. Groups are partitioned into
 *   a smaller number of buckets, using bit shift to get the most signficant digits.
 *   Elements are sorted into these buckets with a parallel counting sort. Then each
 *   bucket contains keys from a subset of groups and can be sorted for that subset
 *   of groups individually.
 */

#include <algorithm>

#include "BLI_array.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_index_range.hh"
#include "BLI_math_base_c.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_task.hh"
#include "BLI_task_size_hints.hh"
#include "BLI_threads.hh"

namespace blender::radix_sort {

/** Number of keys processed as a single chunk for parallelization. */
constexpr int64_t CHUNK_SIZE = 32768;

/** Maximum number of buckets used in the first pass. A small number keeps that pass cache
 * friendly, at the cost of more keys per bucket for the second pass to handle. */
constexpr int64_t MAX_BUCKETS = 512;

/** Number of chunks that #keys_num keys are split into. */
inline int64_t chunks_num(const int64_t keys_num)
{
  return divide_ceil_ul(keys_num, CHUNK_SIZE);
}

/**
 * Compute shift so that `group >> shift` maps a group to a bucket. In the simple
 * case this returns 0 and the group index is the bucket index.
 */
inline int bucket_shift(const int64_t keys_num, const int64_t max_buckets)
{
  int shift = 0;
  while (((keys_num - 1) >> shift) >= max_buckets) {
    shift++;
  }
  return shift;
}

/** Number of buckets for the given number of keys and and #shift. */
inline int64_t buckets_num(const int64_t keys_num, const int shift)
{
  return ((keys_num - 1) >> shift) + 1;
}

/** Parallel count per chunk how many keys fall into each bucket. */
inline void count_buckets_per_chunk(const Span<int> keys,
                                    const int shift,
                                    const int64_t buckets_num,
                                    MutableSpan<int> r_chunk_counts)
{
  const int64_t chunks = r_chunk_counts.size() / buckets_num;
  threading::parallel_for(IndexRange(chunks), 1, [&](const IndexRange range) {
    for (const int64_t chunk : range) {
      MutableSpan<int> counts = r_chunk_counts.slice(chunk * buckets_num, buckets_num);
      counts.fill(0);
      for (const int key : keys.slice_safe(chunk * CHUNK_SIZE, CHUNK_SIZE)) {
        counts[key >> shift]++;
      }
    }
  });
}

/** Prefix sum converting counts into offsets. */
inline void counts_to_offsets(MutableSpan<int> data)
{
  int offset = 0;
  for (const int64_t i : data.index_range().drop_back(1)) {
    const int count = data[i];
    BLI_assert(count >= 0);
    data[i] = offset;
    offset += count;
  }
  data.last() = offset;
}

/** Prefix sum converting per-chunk bucket counts into offsets of each chunk in each bucket. */
inline void chunk_counts_to_offsets(MutableSpan<int> chunk_counts,
                                    MutableSpan<int> r_bucket_counts)
{
  const int64_t buckets_num = r_bucket_counts.size();
  const int64_t chunks = chunk_counts.size() / buckets_num;
  for (const int64_t chunk : IndexRange(chunks)) {
    MutableSpan<int> counts = chunk_counts.slice(chunk * buckets_num, buckets_num);
    for (const int64_t bucket : IndexRange(buckets_num)) {
      const int count = counts[bucket];
      counts[bucket] = r_bucket_counts[bucket];
      r_bucket_counts[bucket] += count;
    }
  }
}

/**
 * Write the a value for each key in #r_partitioned, into the range of the
 * bucket it belongs to.
 *
 * The start position of each bucket is provided as #bucket_offsets, and
 * #chunk_offsets has the offset of each chunk in each bucket.
 *
 * #T and #MakeValue determine the type and value written into #r_partitioned.
 */
template<typename T, typename MakeValue>
inline void partition_into_buckets(const Span<int> keys,
                                   const int shift,
                                   const Span<int> bucket_offsets,
                                   const MakeValue make_value,
                                   MutableSpan<int> chunk_offsets,
                                   MutableSpan<T> r_partitioned)
{
  const int64_t buckets_num = bucket_offsets.size();
  const int64_t chunks = chunk_offsets.size() / buckets_num;
  threading::parallel_for(IndexRange(chunks), 1, [&](const IndexRange range) {
    for (const int64_t chunk : range) {
      MutableSpan<int> current_offsets = chunk_offsets.slice(chunk * buckets_num, buckets_num);
      const int64_t begin = chunk * CHUNK_SIZE;
      const Span<int> chunk_keys = keys.slice_safe(begin, CHUNK_SIZE);
      for (const int64_t i : chunk_keys.index_range()) {
        const int key = chunk_keys[i];
        const int bucket = key >> shift;
        const int current_bucket_offset = bucket_offsets[bucket] + current_offsets[bucket]++;
        r_partitioned[current_bucket_offset] = make_value(begin + i, key);
      }
    }
  });
}

/** Maximum number of groups handled by the single-pass counting sort. */
constexpr int64_t MAX_SINGLE_PASS_GROUPS = 1 << 14;

/**
 * Maximum number of elements sorted with a plain serial counting sort, below which
 * parallel implementations add too much overhead.
 */
constexpr int64_t MAX_SERIAL_SORT_INDICES = 1 << 18;

template<typename GetValue>
inline void sort_into_groups_serial(const Span<int> keys,
                                    const Span<int> known_group_offsets,
                                    const GetValue get_value,
                                    MutableSpan<int> r_offset_data,
                                    MutableSpan<int> r_values)
{
  Span<int> group_offsets = known_group_offsets;
  if (known_group_offsets.is_empty()) {
    r_offset_data.fill(0);
    for (const int group : keys) {
      r_offset_data[group]++;
    }
    counts_to_offsets(r_offset_data);
    group_offsets = r_offset_data.drop_back(1);
  }

  Array<int> current_offsets(group_offsets);
  for (const int64_t i : keys.index_range()) {
    const int current_offset = current_offsets[keys[i]]++;
    r_values[current_offset] = get_value(i);
  }
}

template<typename GetValue>
inline void sort_into_groups_parallel_counting(const Span<int> keys,
                                               const int64_t groups_num,
                                               const Span<int> known_group_offsets,
                                               const GetValue get_value,
                                               MutableSpan<int> r_offset_data,
                                               MutableSpan<int> r_values)
{
  Array<int> chunk_counts(chunks_num(keys.size()) * groups_num);
  count_buckets_per_chunk(keys, 0, groups_num, chunk_counts);

  Span<int> group_offsets = known_group_offsets;
  Array<int> group_counts;
  if (known_group_offsets.is_empty()) {
    r_offset_data.fill(0);
    chunk_counts_to_offsets(chunk_counts, r_offset_data.drop_back(1));
    counts_to_offsets(r_offset_data);
    group_offsets = r_offset_data.drop_back(1);
  }
  else {
    group_counts = Array<int>(groups_num, 0);
    chunk_counts_to_offsets(chunk_counts, group_counts);
  }

  partition_into_buckets<int>(
      keys,
      0,
      group_offsets,
      [&](const int64_t position, const int /*group*/) { return get_value(position); },
      chunk_counts,
      r_values);
}

/* Same as #sort_into_groups_serial, but with bucket offsets. */
inline void sort_bucket_into_groups(const Span<int2> pairs,
                                    const int64_t group_begin,
                                    const int64_t bucket_groups_num,
                                    const int bucket_offset,
                                    const Span<int> known_group_offsets,
                                    MutableSpan<int> offsets_buffer,
                                    MutableSpan<int> r_offset_data,
                                    MutableSpan<int> r_values)
{
  MutableSpan<int> current_offsets = offsets_buffer.take_front(bucket_groups_num);
  if (known_group_offsets.is_empty()) {
    current_offsets.fill(0);
    for (const int2 &pair : pairs) {
      current_offsets[pair.y - group_begin]++;
    }
    int offset = bucket_offset;
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

  for (const int2 &pair : pairs) {
    const int current_offset = current_offsets[pair.y - group_begin]++;
    r_values[current_offset] = pair.x;
  }
}

template<typename GetValue>
inline void sort_into_groups_parallel_radix(const Span<int> keys,
                                            const int64_t groups_num,
                                            const Span<int> known_group_offsets,
                                            const int shift,
                                            const GetValue get_value,
                                            MutableSpan<int> r_offset_data,
                                            MutableSpan<int> r_values)
{
  const bool compute_offsets = known_group_offsets.is_empty();
  const int64_t buckets = buckets_num(groups_num, shift);

  /* Parallel count how many keys fall into each bucket. */
  Array<int> chunk_counts(chunks_num(keys.size()) * buckets);
  count_buckets_per_chunk(keys, shift, buckets, chunk_counts);

  /* Serial prefix sum. */
  Array<int> bucket_offset_data(buckets + 1, 0);
  if (compute_offsets) {
    chunk_counts_to_offsets(chunk_counts, bucket_offset_data.as_mutable_span().drop_back(1));
    counts_to_offsets(bucket_offset_data);
  }
  else {
    for (const int64_t bucket : IndexRange(buckets)) {
      bucket_offset_data[bucket] = known_group_offsets[bucket << shift];
    }
    bucket_offset_data[buckets] = int(keys.size());
    Array<int> bucket_counts(buckets, 0);
    chunk_counts_to_offsets(chunk_counts, bucket_counts);
  }
  const OffsetIndices<int> bucket_offsets(bucket_offset_data.as_span());

  /* Parallel partition keys into their buckets, writing (value, group) into each bucket. */
  Array<int2> pairs(keys.size());
  partition_into_buckets<int2>(
      keys,
      shift,
      bucket_offset_data.as_span().drop_back(1),
      [&](const int64_t position, const int group) { return int2(get_value(position), group); },
      chunk_counts,
      pairs);

  /* Sort every bucket by group. */
  const int64_t groups_per_bucket = int64_t(1) << shift;
  threading::EnumerableThreadSpecific<Array<int>> all_current_offsets(
      [&]() { return Array<int>(groups_per_bucket); });
  threading::parallel_for(
      IndexRange(buckets),
      1,
      [&](const IndexRange range) {
        MutableSpan<int> current_offsets = all_current_offsets.local();
        for (const int64_t bucket : range) {
          const int64_t group_begin = bucket << shift;
          const int64_t bucket_groups_num = std::min(groups_per_bucket, groups_num - group_begin);
          const IndexRange bucket_range = bucket_offsets[bucket];
          sort_bucket_into_groups(pairs.as_span().slice(bucket_range),
                                  group_begin,
                                  bucket_groups_num,
                                  int(bucket_range.start()),
                                  known_group_offsets,
                                  current_offsets,
                                  r_offset_data,
                                  r_values);
        }
      },
      threading::accumulated_task_sizes(
          [&](const IndexRange range) { return bucket_offsets[range].size(); }));

  if (compute_offsets) {
    r_offset_data.last() = int(keys.size());
  }
}

/**
 * Sort values by #keys in ascending order,. #keys must be in the range 0..#groups_num.
 *
 * Values are written into #r_values as `get_value(key_index)`, where #key_index is the
 * index into #keys.
 *
 * If #known_group_offsets is not provided, the offset of each group is computed and
 * stored in #r_offset_data. If it's non-empty, #r_offset_data is unused.
 */
template<typename GetValue>
inline void sort_into_groups(const Span<int> keys,
                             const int64_t groups_num,
                             const Span<int> known_group_offsets,
                             const GetValue get_value,
                             MutableSpan<int> r_offset_data,
                             MutableSpan<int> r_values)
{
  BLI_assert(!keys.is_empty());
  BLI_assert(known_group_offsets.is_empty() ? r_offset_data.size() == groups_num + 1 :
                                              known_group_offsets.size() == groups_num);

  if (keys.size() < MAX_SERIAL_SORT_INDICES || BLI_system_thread_count() < 4) {
    sort_into_groups_serial(keys, known_group_offsets, get_value, r_offset_data, r_values);
    return;
  }

  const int shift = bucket_shift(groups_num, MAX_BUCKETS);
  if (groups_num <= MAX_SINGLE_PASS_GROUPS || shift == 0) {
    sort_into_groups_parallel_counting(
        keys, groups_num, known_group_offsets, get_value, r_offset_data, r_values);
  }
  else {
    sort_into_groups_parallel_radix(
        keys, groups_num, known_group_offsets, shift, get_value, r_offset_data, r_values);
  }
}

}  // namespace blender::radix_sort
