/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>

#include "BLI_array_utils.hh"
#include "BLI_generic_span.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_rand.hh"
#include "BLI_task.hh"

#include "GEO_abstract_kd_bucket_hierarchy.hh"
#include "GEO_bounding_sphere.hh"
#include "GEO_fast_multipole_method.hh"

namespace blender {

class ShiftedRange {
 public:
  int start;
  int size;

  IndexRange index_range() const
  {
    return IndexRange(size);
  }

  int64_t operator[](int64_t index) const
  {
    BLI_assert(index >= 0);
    BLI_assert(index < this->size);
    return start + index;
  }
};

}  // namespace blender

namespace blender::fast_math {

static BLI_NOINLINE void distance_to_n_squared(const Span<float> src_a_x,
                                               const Span<float> src_a_y,
                                               const Span<float> src_a_z,
                                               const float3 src_b_xyz,
                                               MutableSpan<float> dst)
{
  BLI_assert(src_a_x.size() == src_a_y.size());
  BLI_assert(src_a_x.size() == src_a_z.size());
  BLI_assert(src_a_x.size() == dst.size());

  for (const int i : src_a_x.index_range()) {
    dst[i] = math::distance_squared(float3(src_a_x[i], src_a_y[i], src_a_z[i]), src_b_xyz);
  }
}

static BLI_NOINLINE void squared_distances_row_major(const Span<float> row_x,
                                                     const Span<float> row_y,
                                                     const Span<float> row_z,
                                                     const Span<float> col_x,
                                                     const Span<float> col_y,
                                                     const Span<float> col_z,
                                                     MutableSpan<float> distances)
{
  BLI_assert(row_x.size() == row_y.size());
  BLI_assert(row_x.size() == row_z.size());

  BLI_assert(col_x.size() == col_y.size());
  BLI_assert(col_x.size() == col_z.size());

  BLI_assert(distances.size() == row_x.size() * col_z.size());

  for (const int row_index : row_z.index_range()) {
    const float3 row_xyz = {row_x[row_index], row_y[row_index], row_z[row_index]};
    for (const int col_index : col_z.index_range()) {
      const float3 col_xyz = {col_x[col_index], col_y[col_index], col_z[col_index]};
      const float3 batch = math::square(row_xyz - col_xyz);
      distances[row_index * col_z.size() + col_index] = math::reduce_add(batch);
    }
  }
}

static BLI_NOINLINE void zero_if_index_in_range_row_major(const Span<int> row_indices,
                                                          const ShiftedRange col_range,
                                                          MutableSpan<float> rows_and_cols)
{
  BLI_assert(col_range.size * row_indices.size() == rows_and_cols.size());

  for (const int row_index : row_indices.index_range()) {
    const int col_index = row_indices[row_index] - col_range.start;
    if (0 <= col_index && col_index < col_range.size) {
      rows_and_cols[row_index * col_range.size + col_index] = 0.0f;
    }
  }
}

static BLI_NOINLINE void product_reduce_row_major(const Span<float> rows_and_cols,
                                                  const Span<float> col_values,
                                                  MutableSpan<float> row_values)
{
  BLI_assert(rows_and_cols.size() == col_values.size() * row_values.size());

  for (const int row_index : row_values.index_range()) {
    float row_accum = 0.0f;
    for (const int col_index : col_values.index_range()) {
      row_accum += rows_and_cols[row_index * col_values.size() + col_index] *
                   col_values[col_index];
    }
    row_values[row_index] += row_accum;
  }
}

template<typename T>
static BLI_NOINLINE void scatter(const Span<T> src, const Span<int> indices, MutableSpan<T> dst)
{
  BLI_assert(src.size() == indices.size());
  for (const int i : src.index_range()) {
    dst[indices[i]] = src[i];
  }
}

template<typename T>
static BLI_NOINLINE void gather(const Span<T> src, const Span<int> indices, MutableSpan<T> dst)
{
  BLI_assert(dst.size() == indices.size());
  for (const int i : dst.index_range()) {
    dst[i] = src[indices[i]];
  }
}

template<typename T>
static BLI_NOINLINE void parition_as_gather(MutableSpan<T> values,
                                            const Span<int> front_indices,
                                            const Span<int> back_indices,
                                            MutableSpan<T> buffer)
{
  BLI_assert(front_indices.size() == back_indices.size());
  BLI_assert(front_indices.size() + back_indices.size() == buffer.size());
  BLI_assert(values.size() >= buffer.size());

  gather<T>(values, front_indices, buffer.take_front(front_indices.size()));
  gather<T>(values, back_indices, buffer.drop_front(back_indices.size()));

  scatter<T>(buffer.take_front(back_indices.size()), back_indices, values);
  scatter<T>(buffer.drop_front(front_indices.size()), front_indices, values);
}

template<typename T>
static BLI_NOINLINE void parition_as_gather_front_only(MutableSpan<T> values,
                                                       const Span<int> front_indices,
                                                       const Span<int> back_indices,
                                                       MutableSpan<T> buffer)
{
  BLI_assert(front_indices.size() == back_indices.size());
  BLI_assert(front_indices.size() + back_indices.size() == buffer.size());
  BLI_assert(values.size() >= buffer.size());

  gather<T>(values, front_indices, buffer.take_front(front_indices.size()));
  scatter<T>(buffer.take_front(back_indices.size()), back_indices, values);
}

static BLI_NOINLINE int count_floats_less_than(const Span<float> values, float min_predicate_value)
{
  return std::count_if(
      values.begin(), values.end(), [&](const float item) { return item < min_predicate_value; });
}

static BLI_NOINLINE std::pair<Span<int>, Span<int>> predicate_partition_indices_float_cmp(
    MutableSpan<int> indices_buffer,
    const Span<float> predicates,
    const float min_predicate_value,
    const int total_front_size)
{
  BLI_assert(total_front_size > 0);
  BLI_assert(total_front_size <= predicates.size());
  BLI_assert(indices_buffer.size() <= predicates.size());

  const IndexRange front_range(total_front_size);
  const IndexRange back_range = predicates.index_range().drop_front(total_front_size);

  const auto front_indices = std::copy_if(
      front_range.begin(), front_range.end(), indices_buffer.begin(), [&](const int index) {
        return predicates[index] >= min_predicate_value;
      });

  const auto back_indices = std::copy_if(
      back_range.begin(), back_range.end(), front_indices, [&](const int index) {
        return predicates[index] < min_predicate_value;
      });

  const int front_size = std::distance(indices_buffer.begin(), front_indices);
  const int back_size = std::distance(front_indices, back_indices);

  BLI_assert(front_size == back_size);
  return {indices_buffer.slice(0, front_size), indices_buffer.slice(front_size, back_size)};
}

static BLI_NOINLINE void mul_n_add_to(MutableSpan<float> dst,
                                      const Span<float> src,
                                      const float value)
{
  BLI_assert(dst.size() == src.size());
  for (const int i : dst.index_range()) {
    dst[i] += value * src[i];
  }
}

static BLI_NOINLINE void sqrt_n_add_single(MutableSpan<float> values, const float offset_value)
{
  for (float &value : values) {
    value = math::sqrt(value) + offset_value;
  }
}

}  // namespace blender::fast_math

namespace blender::geometry::fmm {

template<typename T> static T round_for(const T value, const T round_cell)
{
  return (value / round_cell) * round_cell;
}

template<typename T> static T round_up_for(const T value, const T round_cell)
{
  BLI_assert(round_cell > 0);
  return ((value + round_cell - T(1)) / round_cell) * round_cell;
}

template<typename T> static bool all_same_size(const Span<T> items)
{
  BLI_assert(!items.is_empty());
  return std::all_of(items.begin() + 1, items.end(), [&](const auto &item) {
    return items.first().size() == item.size();
  });
}

void akdbh_accumulate_in(const OffsetIndices<int> buckets_offsets,
                         const int total_depth,
                         const Span<float> src_joints_min_distance,
                         const Span<float3> src_joints_centre,
                         const Span<Span<float>> src_joints_value,
                         const std::array<Span<float>, 3> src_bucket_position,
                         const Span<Span<float>> src_bucket_value,
                         const int power_value,
                         const float offset_value,
                         const std::array<Span<float>, 3> sample_position,
                         Span<MutableSpan<float>> dst_buckets_data,
                         const std::optional<IndexRange> sampler_to_bucket_range)
{
  {
    BLI_assert(src_joints_value.size() == src_bucket_value.size());
    BLI_assert(src_joints_value.size() == dst_buckets_data.size());

    BLI_assert(all_same_size(Span(src_bucket_position)));
    BLI_assert(all_same_size(Span(sample_position)));

    BLI_assert(all_same_size(src_joints_value));
    BLI_assert(all_same_size(src_bucket_value));
    BLI_assert(all_same_size(dst_buckets_data));

    BLI_assert(buckets_offsets.total_size() == src_bucket_position[0].size());

    BLI_assert(src_joints_min_distance.size() == src_joints_centre.size());
    BLI_assert(src_joints_min_distance.size() == src_joints_value[0].size());

    BLI_assert(src_bucket_position[0].size() == src_bucket_value[0].size());

    BLI_assert(dst_buckets_data[0].size() == sample_position[0].size());
    BLI_assert(!sampler_to_bucket_range.has_value() ||
               sampler_to_bucket_range->size() == dst_buckets_data[0].size());
    BLI_assert(!sampler_to_bucket_range.has_value() ||
               src_bucket_position[0].index_range().contains(*sampler_to_bucket_range));
  }

  const bool has_offset = offset_value != 0.0f;

  const int batch_size = sample_position[0].size();

  const int data_axes_num = src_bucket_value.size();

  Array<float, 0> sampler_position_data(batch_size * 3);
  std::array<MutableSpan<float>, 3> batch_positions_data;
  for (const int axis_i : IndexRange(3)) {
    batch_positions_data[axis_i] = sampler_position_data.as_mutable_span().slice(
        batch_size * axis_i, batch_size);
    batch_positions_data[axis_i].copy_from(sample_position[axis_i]);
  }

  Array<float, 0> sampler_value_data(batch_size * data_axes_num);
  Array<MutableSpan<float>, 3> batch_values_data(data_axes_num);
  for (const int axis_i : IndexRange(data_axes_num)) {
    batch_values_data[axis_i] = sampler_value_data.as_mutable_span().slice(batch_size * axis_i,
                                                                           batch_size);
    batch_values_data[axis_i].fill(0);
  }

  Array<int, 0> sampler_mapping_data(batch_size * 3);

  MutableSpan<int> batch_indices_data = sampler_mapping_data.as_mutable_span().slice(
      batch_size * 0, batch_size);
  MutableSpan<int> partition_indices_buffer = sampler_mapping_data.as_mutable_span().slice(
      batch_size * 1, batch_size);
  MutableSpan<int> partition_buffer_data = sampler_mapping_data.as_mutable_span().slice(
      batch_size * 2, batch_size);

  array_utils::fill_index_range<int>(batch_indices_data, 0);

  Vector<float, 0> batch_distances_buffer(batch_size);

  Vector<float, 0> bucket_position_data;

  Vector<int, 32> depth_stack({0});
  Vector<int, 32> joint_stack({0});
  Vector<int, 32> prefix_to_visit_stack({batch_size});

  RandomNumberGenerator unbiased_order_generator({0});
  const auto push_childs_of = [&](const int depth_i, const int joint_i, const int prefix_size) {
    BLI_assert(depth_i < total_depth - 1);

    bool first_on_top = true;
    if ((depth_i < total_depth / 2) && (total_depth > 10)) {
      first_on_top = (unbiased_order_generator.get_int32() & 1) == 1;
    }
    const int first_child_i = joint_i * 2 + (first_on_top ? 1 : 0);
    const int second_child_i = joint_i * 2 + (first_on_top ? 0 : 1);

    depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
    joint_stack.extend_unchecked({first_child_i, second_child_i});
    prefix_to_visit_stack.extend_unchecked({prefix_size, prefix_size});
  };

  while (!depth_stack.is_empty()) {
    const int prefix_to_visit = prefix_to_visit_stack.pop_last();
    const int depth_i = depth_stack.pop_last();
    const int joint_i = joint_stack.pop_last();
    const IndexRange joints_range = akdbh::joints_range_at_depth(depth_i);
    const int joint_index = joints_range[joint_i];

    const bool leaf_joint = depth_i == total_depth - 1;

    const IndexRange joint_buckets_range = akdbh::joint_buckets_range_at_depth(
        total_depth, depth_i, joint_i);
    const IndexRange joint_buckets = buckets_offsets[joint_buckets_range];
    const bool totally_inside_of_joint = sampler_to_bucket_range.has_value() &&
                                         joint_buckets.contains(*sampler_to_bucket_range);

    const float3 joint_position = src_joints_centre[joint_index];
    const float joint_min_distance = src_joints_min_distance[joint_index];

    const MutableSpan<float> batch_positions_x = batch_positions_data[0].take_front(
        prefix_to_visit);
    const MutableSpan<float> batch_positions_y = batch_positions_data[1].take_front(
        prefix_to_visit);
    const MutableSpan<float> batch_positions_z = batch_positions_data[2].take_front(
        prefix_to_visit);

    if (!totally_inside_of_joint) {
      batch_distances_buffer.reinitialize(prefix_to_visit);

      fast_math::distance_to_n_squared(batch_positions_x,
                                       batch_positions_y,
                                       batch_positions_z,
                                       joint_position,
                                       batch_distances_buffer);

      if (has_offset) {
        fast_math::sqrt_n_add_single(batch_distances_buffer.as_mutable_span(), offset_value);
      }
    }
    const float min_distance_to_joint = has_offset ? joint_min_distance :
                                                     math::square(joint_min_distance);
    const int total_to_pass_to_childs = totally_inside_of_joint ?
                                            prefix_to_visit :
                                            fast_math::count_floats_less_than(
                                                batch_distances_buffer, min_distance_to_joint);

    const bool all_pass_to_childs = total_to_pass_to_childs == prefix_to_visit;
    if (all_pass_to_childs && !leaf_joint) {
      push_childs_of(depth_i, joint_i, prefix_to_visit);
      continue;
    }

    const bool all_end_on_joint = total_to_pass_to_childs == 0;
    if (!all_pass_to_childs) {
      const MutableSpan<int> batch_indices = batch_indices_data.take_front(prefix_to_visit);
      const MutableSpan<int> partition = partition_indices_buffer.take_front(prefix_to_visit);

#ifndef NDEBUG
      Array<int> partition_to_check;
#endif

      std::optional<std::pair<Span<int>, Span<int>>> partition_mapping;
      std::optional<MutableSpan<int>> partition_buffer;
      if (!all_end_on_joint) {
        partition_mapping = fast_math::predicate_partition_indices_float_cmp(
            partition, batch_distances_buffer, min_distance_to_joint, total_to_pass_to_childs);
        partition_buffer = partition_buffer_data.take_front(partition_mapping->first.size() +
                                                            partition_mapping->second.size());

#ifndef NDEBUG
        partition_to_check = partition.as_span();
#endif

        fast_math::parition_as_gather_front_only(batch_distances_buffer.as_mutable_span(),
                                                 partition_mapping->first,
                                                 partition_mapping->second,
                                                 partition_buffer->cast<float>());
        std::copy(batch_distances_buffer.begin() + total_to_pass_to_childs,
                  batch_distances_buffer.end(),
                  batch_distances_buffer.begin());
      }

      {
        MutableSpan<float> values = batch_distances_buffer.as_mutable_span().take_front(
            prefix_to_visit - total_to_pass_to_childs);
        if (has_offset) {
          std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
            return math::rcp(math::pow<float>(value, power_value));
          });
        }
        else {
          std::transform(values.begin(), values.end(), values.begin(), [&](const float value) {
            return math::rcp(math::pow<float>(value, power_value * 0.5f));
          });
        }
      }

      if (!all_end_on_joint) {
        for (const int data_i : IndexRange(data_axes_num)) {
          const MutableSpan<float> batch_values = batch_values_data[data_i].take_front(
              prefix_to_visit);
          BLI_assert(partition_mapping.has_value());
          fast_math::parition_as_gather(batch_values,
                                        partition_mapping->first,
                                        partition_mapping->second,
                                        partition_buffer->cast<float>());
        }
      }

      for (const int data_i : IndexRange(data_axes_num)) {
        const float data_value = src_joints_value[data_i][joint_index];
        const MutableSpan<float> batch_values = batch_values_data[data_i].take_front(
            prefix_to_visit);
        fast_math::mul_n_add_to(
            batch_values.drop_front(total_to_pass_to_childs),
            batch_distances_buffer.as_span().take_front(prefix_to_visit - total_to_pass_to_childs),
            data_value);
      }

      if (!all_end_on_joint) {
        BLI_assert(partition_mapping.has_value());

        fast_math::parition_as_gather(
            batch_indices, partition_mapping->first, partition_mapping->second, *partition_buffer);
        fast_math::parition_as_gather(batch_positions_x,
                                      partition_mapping->first,
                                      partition_mapping->second,
                                      partition_buffer->cast<float>());
        fast_math::parition_as_gather(batch_positions_y,
                                      partition_mapping->first,
                                      partition_mapping->second,
                                      partition_buffer->cast<float>());
        fast_math::parition_as_gather(batch_positions_z,
                                      partition_mapping->first,
                                      partition_mapping->second,
                                      partition_buffer->cast<float>());

        BLI_assert(partition_to_check.as_span() == partition.as_span());
      }
    }

    if (!leaf_joint) {
      if (all_end_on_joint) {
        continue;
      }

      push_childs_of(depth_i, joint_i, total_to_pass_to_childs);
      continue;
    }

    const IndexRange joint_bucket = buckets_offsets[joint_i];

    const int bucket_size = joint_bucket.size();
    bucket_position_data.reinitialize(bucket_size * 3);
    for (const int axis_i : IndexRange(3)) {
      bucket_position_data.as_mutable_span()
          .slice(bucket_size * axis_i, bucket_size)
          .copy_from(src_bucket_position[axis_i].slice(joint_bucket));
    }

    const Span<float> bucket_positions_x = bucket_position_data.as_mutable_span().slice(
        bucket_size * 0, bucket_size);
    const Span<float> bucket_positions_y = bucket_position_data.as_mutable_span().slice(
        bucket_size * 1, bucket_size);
    const Span<float> bucket_positions_z = bucket_position_data.as_mutable_span().slice(
        bucket_size * 2, bucket_size);

    const Span<float> batch_x = batch_positions_x.take_front(total_to_pass_to_childs);
    const Span<float> batch_y = batch_positions_y.take_front(total_to_pass_to_childs);
    const Span<float> batch_z = batch_positions_z.take_front(total_to_pass_to_childs);

    batch_distances_buffer.reinitialize(bucket_size * total_to_pass_to_childs);
    const MutableSpan<float> distance_table = batch_distances_buffer.as_mutable_span();

#ifndef NDEBUG
    distance_table.fill(-1.0f);
#endif

    fast_math::squared_distances_row_major(batch_x,
                                           batch_y,
                                           batch_z,
                                           bucket_positions_x,
                                           bucket_positions_y,
                                           bucket_positions_z,
                                           distance_table);

    BLI_assert(!distance_table.as_span().contains(-1.0f));

    if (has_offset) {
      fast_math::sqrt_n_add_single(distance_table, offset_value);
      std::transform(
          distance_table.begin(),
          distance_table.end(),
          distance_table.begin(),
          [&](const float value) { return math::safe_rcp(math::pow<float>(value, power_value)); });
    }
    else {
      std::transform(distance_table.begin(),
                     distance_table.end(),
                     distance_table.begin(),
                     [&](const float value) {
                       return math::safe_rcp(math::pow<float>(value, power_value * 0.5f));
                     });
    }

    if (sampler_to_bucket_range.has_value()) {
      const IndexRange range_of_samplers = *sampler_to_bucket_range;
      if (!range_of_samplers.intersect(joint_bucket).is_empty()) {
        const Span<int> batch_indices = batch_indices_data.take_front(total_to_pass_to_childs);

        const int from_bucket_to_sampler_offset = joint_bucket.start() - range_of_samplers.start();
        fast_math::zero_if_index_in_range_row_major(
            batch_indices,
            ShiftedRange{from_bucket_to_sampler_offset, bucket_size},
            distance_table);
      }
    }

    for (const int data_i : IndexRange(data_axes_num)) {
      const Span<float> bucket_values = src_bucket_value[data_i].slice(joint_bucket);
      const MutableSpan<float> batch_values = batch_values_data[data_i].take_front(
          total_to_pass_to_childs);
      fast_math::product_reduce_row_major(distance_table, bucket_values, batch_values);
    }
  }

  for (const int data_i : IndexRange(data_axes_num)) {
    array_utils::scatter<float, int>(batch_values_data[data_i].as_span(),
                                     batch_indices_data.as_span(),
                                     dst_buckets_data[data_i]);
  }
}

}  // namespace blender::geometry::fmm

namespace blender {

void transpose(const Span<float3> src, Span<MutableSpan<float>> dst)
{
  BLI_assert(dst.size() == decltype(src)::value_type::type_length);
  BLI_assert(std::all_of(dst.begin(), dst.end(), [&](const MutableSpan<float> span) {
    return span.size() == src.size();
  }));

  threading::parallel_for(src.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[0][i] = src[i].x;
      dst[1][i] = src[i].y;
      dst[2][i] = src[i].z;
    }
  });
}

void transpose(const Span<Span<float>> src, MutableSpan<float3> dst)
{
  BLI_assert(src.size() == decltype(dst)::value_type::type_length);
  BLI_assert(std::all_of(
      src.begin(), src.end(), [&](const Span<float> span) { return span.size() == dst.size(); }));

  threading::parallel_for(dst.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[i].x = src[0][i];
      dst[i].y = src[1][i];
      dst[i].z = src[2][i];
    }
  });
}

void transpose_gather(const Span<float3> src,
                      const Span<int> indices,
                      Span<MutableSpan<float>> dst)
{
  BLI_assert(dst.size() == decltype(src)::value_type::type_length);
  BLI_assert(std::all_of(dst.begin(), dst.end(), [&](const MutableSpan<float> span) {
    return span.size() == indices.size();
  }));

  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[0][i] = src[indices[i]].x;
      dst[1][i] = src[indices[i]].y;
      dst[2][i] = src[indices[i]].z;
    }
  });
}

void transpose_gather(const Span<Span<float>> src,
                      const Span<int> indices,
                      MutableSpan<float3> dst)
{
  BLI_assert(src.size() == decltype(dst)::value_type::type_length);
  BLI_assert(dst.size() == indices.size());

  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[i].x = src[0][indices[i]];
      dst[i].y = src[1][indices[i]];
      dst[i].z = src[2][indices[i]];
    }
  });
}

void transpose_gather(const Span<float3> src, const IndexMask mask, Span<MutableSpan<float>> dst)
{
  BLI_assert(dst.size() == decltype(src)::value_type::type_length);
  BLI_assert(std::all_of(dst.begin(), dst.end(), [&](const MutableSpan<float> span) {
    return span.size() == mask.size();
  }));

  mask.foreach_index_optimized<int>(GrainSize(4096), [&](const int i, const int pos) {
    dst[0][pos] = src[i].x;
    dst[1][pos] = src[i].y;
    dst[2][pos] = src[i].z;
  });
}

void transpose_gather(const Span<Span<float>> src, const IndexMask mask, MutableSpan<float3> dst)
{
  BLI_assert(src.size() == decltype(dst)::value_type::type_length);
  BLI_assert(dst.size() == mask.size());

  mask.foreach_index_optimized<int>(GrainSize(4096), [&](const int i, const int pos) {
    dst[pos].x = src[0][i];
    dst[pos].y = src[1][i];
    dst[pos].z = src[2][i];
  });
}

void transpose_scatter(const Span<float3> src,
                       const Span<int> indices,
                       Span<MutableSpan<float>> dst)
{
  BLI_assert(dst.size() == decltype(src)::value_type::type_length);
  BLI_assert(src.size() == indices.size());

  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[0][indices[i]] = src[i].x;
      dst[1][indices[i]] = src[i].y;
      dst[2][indices[i]] = src[i].z;
    }
  });
}

void transpose_scatter(const Span<Span<float>> src,
                       const Span<int> indices,
                       MutableSpan<float3> dst)
{
  BLI_assert(src.size() == decltype(dst)::value_type::type_length);
  BLI_assert(std::all_of(src.begin(), src.end(), [&](const Span<float> span) {
    return span.size() == indices.size();
  }));

  threading::parallel_for(indices.index_range(), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      dst[indices[i]].x = src[0][i];
      dst[indices[i]].y = src[1][i];
      dst[indices[i]].z = src[2][i];
    }
  });
}

void transpose_scatter(const Span<float3> src, const IndexMask mask, Span<MutableSpan<float>> dst)
{
  BLI_assert(dst.size() == decltype(src)::value_type::type_length);
  BLI_assert(src.size() == mask.size());

  mask.foreach_index_optimized<int>(GrainSize(4096), [&](const int i, const int pos) {
    dst[0][i] = src[pos].x;
    dst[1][i] = src[pos].y;
    dst[2][i] = src[pos].z;
  });
}

void transpose_scatter(const Span<Span<float>> src, const IndexMask mask, MutableSpan<float3> dst)
{
  BLI_assert(src.size() == decltype(dst)::value_type::type_length);
  BLI_assert(std::all_of(
      src.begin(), src.end(), [&](const Span<float> span) { return span.size() == mask.size(); }));

  mask.foreach_index_optimized<int>(GrainSize(4096), [&](const int i, const int pos) {
    dst[i].x = src[0][pos];
    dst[i].y = src[1][pos];
    dst[i].z = src[2][pos];
  });
}

}  // namespace blender
