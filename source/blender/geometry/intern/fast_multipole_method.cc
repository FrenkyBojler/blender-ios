/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <iostream>
#include <sstream>

#include "BLI_timeit.hh"

#include "BLI_allocator.hh"

#include "BLI_function_ref.hh"
#include "BLI_generic_span.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task.hh"

#include "GEO_abstract_kd_bucket_hierarchy.hh"
#include "GEO_bounding_sphere.hh"
#include "GEO_fast_multipole_method.hh"

#include "fast_math.h"

#include "BLI_math_bits.h"

namespace blender::geometry::fmm {

static FunctionRef<void(int, MutableSpan<float>)> powered_rcp_for_values_old(const int power_value)
{
  switch (power_value) {
    case 0:
      return [](int /*power_value*/, MutableSpan<float> values) { values.fill(1.0f); };
    case 1:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          return math::safe_rcp(value);
        });
      };
    case 2:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          return math::safe_rcp(value * value);
        });
      };
    case 3:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          return math::safe_rcp(value * value * value);
        });
      };
    case 4:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          const float squared = math::square(value);
          return math::safe_rcp(squared * squared);
        });
      };
    case 5:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          const float squared = math::square(value);
          return math::safe_rcp(squared * squared * value);
        });
      };
    case 6:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          const float squared = math::square(value);
          return math::safe_rcp(squared * squared * squared);
        });
      };
    case 7:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          const float squared = math::square(value);
          const float fourth_degree = math::square(squared);
          return math::safe_rcp(fourth_degree * squared * value);
        });
      };
    case 8:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          const float squared = math::square(value);
          const float fourth_degree = math::square(squared);
          return math::safe_rcp(fourth_degree * fourth_degree);
        });
      };
    case 9:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          const float squared = math::square(value);
          const float fourth_degree = math::square(squared);
          return math::safe_rcp(fourth_degree * fourth_degree * value);
        });
      };
    case 10:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          const float squared = math::square(value);
          const float fourth_degree = math::square(squared);
          return math::safe_rcp(fourth_degree * fourth_degree * squared);
        });
      };
    case 11:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          const float squared = math::square(value);
          const float fourth_degree = math::square(squared);
          return math::safe_rcp(fourth_degree * fourth_degree * squared * value);
        });
      };
    case 12:
      return [](int /*power_value*/, MutableSpan<float> values) {
        std::transform(values.begin(), values.end(), values.begin(), [](const float value) {
          const float squared = math::square(value);
          const float fourth_degree = math::square(squared);
          return math::safe_rcp(fourth_degree * fourth_degree * fourth_degree);
        });
      };
    default:
      return [](const int power_value, MutableSpan<float> values) {
        const float power_factor = float(-power_value);
        std::transform(
            values.begin(), values.end(), values.begin(), [power_factor](const float value) {
              return math::pow(value, power_factor);
            });
      };
  }
}

static FunctionRef<void(int, MutableSpan<float>)> powered_rcp_for_values(const int power_value)
{
  switch (power_value) {
    case 0:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_0_rpow_n(values.begin(), values.size());
      };
    case 1:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_1_rpow_n(values.begin(), values.size());
      };
    case 2:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_2_rpow_n(values.begin(), values.size());
      };
    case 3:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_3_rpow_n(values.begin(), values.size());
      };
    case 4:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_4_rpow_n(values.begin(), values.size());
      };
    case 5:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_5_rpow_n(values.begin(), values.size());
      };
    case 6:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_6_rpow_n(values.begin(), values.size());
      };
    case 7:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_7_rpow_n(values.begin(), values.size());
      };
    case 8:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_8_rpow_n(values.begin(), values.size());
      };
    case 9:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_9_rpow_n(values.begin(), values.size());
      };
    case 10:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_10_rpow_n(values.begin(), values.size());
      };
    case 11:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_11_rpow_n(values.begin(), values.size());
      };
    case 12:
      return [](int /*power_value*/, MutableSpan<float> values) {
        ispc::fixed_safe_12_rpow_n(values.begin(), values.size());
      };
    default:
      return [](const int power_value, MutableSpan<float> values) {
        const float power_factor = float(power_value);
        std::transform(
            values.begin(), values.end(), values.begin(), [power_factor](const float value) {
              return math::safe_rcp(math::pow(value, power_factor));
            });
      };
  }
}

template<typename T> static T round_for(const T value, const T round_cell)
{
  return (value / round_cell) * round_cell;
}

template<typename T>  static T round_up_for(const T value, const T round_cell)
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

static constexpr int sse_min_alignment = 16;

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
    BLI_assert(!sampler_to_bucket_range.has_value() || sampler_to_bucket_range->size() == dst_buckets_data[0].size());
    BLI_assert(!sampler_to_bucket_range.has_value() || src_bucket_position[0].index_range().contains(*sampler_to_bucket_range));
  }
  const FunctionRef<void(int, MutableSpan<float>)> distance_invertion = powered_rcp_for_values(power_value);

  const int batch_size = sample_position[0].size();
  const int aligned_batch_size = round_up_for(batch_size, sse_min_alignment);
  const int data_axes_num = src_bucket_value.size();

  Array<float, 0, GuardedAlignedAllocator<sse_min_alignment>> sampler_position_data(aligned_batch_size * 3);
  std::array<MutableSpan<float>, 3> batch_positions_data;
  for (const int axis_i : IndexRange(3)) {
    batch_positions_data[axis_i] = sampler_position_data.as_mutable_span().slice(aligned_batch_size * axis_i, batch_size);
    batch_positions_data[axis_i].copy_from(sample_position[axis_i]);
  }

  Array<float, 0, GuardedAlignedAllocator<sse_min_alignment>> sampler_value_data(aligned_batch_size * data_axes_num);
  Array<MutableSpan<float>, 3> batch_values_data(data_axes_num);
  for (const int axis_i : IndexRange(data_axes_num)) {
    batch_values_data[axis_i] = sampler_value_data.as_mutable_span().slice(aligned_batch_size * axis_i, batch_size);
    batch_values_data[axis_i].fill(0);
  }

  Array<int, 0, GuardedAlignedAllocator<sse_min_alignment>> sampler_mapping_data(aligned_batch_size * 3);

  MutableSpan<int> batch_indices_data = sampler_mapping_data.as_mutable_span().slice(aligned_batch_size * 0, batch_size);
  MutableSpan<int> partition_indices_buffer = sampler_mapping_data.as_mutable_span().slice(aligned_batch_size * 1, batch_size);
  MutableSpan<int> partition_buffer_data = sampler_mapping_data.as_mutable_span().slice(aligned_batch_size * 2, batch_size);

  array_utils::fill_index_range<int>(batch_indices_data, 0);

  Vector<float, 0, GuardedAlignedAllocator<sse_min_alignment>> batch_distances_buffer(batch_size);

  Vector<float, 0, GuardedAlignedAllocator<sse_min_alignment>> bucket_position_data;

  static_assert(sizeof(int) == sizeof(float), "Some ISPC functions are reused for int and float data");
  static_assert(alignof(int) == alignof(float), "Some ISPC functions are reused for int and float data");

  Vector<int, 32> depth_stack({0});
  Vector<int, 32> joint_stack({0});
  Vector<int, 32> prefix_to_visit_stack({batch_size});

  while (!depth_stack.is_empty()) {
    const int prefix_to_visit = prefix_to_visit_stack.pop_last();
    const int depth_i = depth_stack.pop_last();
    const int joint_i = joint_stack.pop_last();
    const IndexRange joints_range = akdbh::joints_range_at_depth(depth_i);
    const int joint_index = joints_range[joint_i];

    const bool leaf_joint = depth_i == total_depth - 1;

    const float3 joint_position = src_joints_centre[joint_index];
    const float joint_min_distance = src_joints_min_distance[joint_index];

    const MutableSpan<float> batch_positions_x = batch_positions_data[0].take_front(prefix_to_visit);
    const MutableSpan<float> batch_positions_y = batch_positions_data[1].take_front(prefix_to_visit);
    const MutableSpan<float> batch_positions_z = batch_positions_data[2].take_front(prefix_to_visit);

    batch_distances_buffer.reinitialize(prefix_to_visit);
    ispc::distance_to_n(batch_positions_x.data(),
                        batch_positions_y.data(),
                        batch_positions_z.data(),
                        joint_position,
                        prefix_to_visit,
                        batch_distances_buffer.data());

    const int total_to_pass_to_childs = ispc::count_floats_less_than(batch_distances_buffer.data(), joint_min_distance - offset_value, prefix_to_visit);

    const bool all_pass_to_childs = total_to_pass_to_childs == prefix_to_visit;
    if (all_pass_to_childs && !leaf_joint) {
      depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
      joint_stack.extend_unchecked({joint_i * 2 + 1, joint_i * 2 + 0});
      prefix_to_visit_stack.extend_unchecked({prefix_to_visit, prefix_to_visit});
      continue;
    }

    const bool all_end_on_joint = total_to_pass_to_childs == 0;
    if (!all_pass_to_childs) {
      const MutableSpan<int> batch_indices = batch_indices_data.take_front(prefix_to_visit);
      const MutableSpan<int> partition = partition_indices_buffer.take_front(prefix_to_visit);

#ifndef NDEBUG
      Array<int> partition_to_check;
#endif

      int pertition_mapping_total = -1;
      if (!all_end_on_joint) {
        pertition_mapping_total = ispc::predicate_partition_indices_float_cmp(
            partition.data(),
            batch_distances_buffer.data(),
            prefix_to_visit,
            joint_min_distance - offset_value,
            total_to_pass_to_childs);

#ifndef NDEBUG
        partition_to_check = partition.as_span();
#endif

        ispc::parition_as_gather_front(batch_distances_buffer.as_mutable_span().cast<int>().data(),
                                       partition.data(),
                                       partition_buffer_data.data(),
                                       pertition_mapping_total,
                                       prefix_to_visit,
                                       total_to_pass_to_childs);
      }

      for (const int i : IndexRange(prefix_to_visit - total_to_pass_to_childs)) {
        batch_distances_buffer[i] += offset_value;
      }

      distance_invertion(power_value, batch_distances_buffer.as_mutable_span().take_front(prefix_to_visit - total_to_pass_to_childs));

      if (!all_end_on_joint) {
        for (const int data_i : IndexRange(data_axes_num)) {
          const MutableSpan<float> batch_values = batch_values_data[data_i].take_front(prefix_to_visit);
          BLI_assert(pertition_mapping_total != -1);
          ispc::parition_as_gather(batch_values.cast<int>().data(),
                                   partition.data(),
                                   partition_buffer_data.data(),
                                   pertition_mapping_total);
        }
      }

      for (const int data_i : IndexRange(data_axes_num)) {
        const float data_value = src_joints_value[data_i][joint_index];
        const MutableSpan<float> batch_values = batch_values_data[data_i].take_front(prefix_to_visit);
        ispc::mul_n_add_to(batch_values.drop_front(total_to_pass_to_childs).data(),
                           batch_distances_buffer.data(),
                           data_value,
                           batch_values.drop_front(total_to_pass_to_childs).size());
      }

      if (!all_end_on_joint) {
        BLI_assert(pertition_mapping_total != -1);
        BLI_assert(std::all_of(partition.begin(), partition.begin() + pertition_mapping_total, [&](const int i) {
          BLI_assert(i >= 0);
          BLI_assert(i < batch_size);
          return IndexRange(batch_indices.size()).contains(i);
        }));
        ispc::parition_as_gather(batch_indices.data(),
                                 partition.data(),
                                 partition_buffer_data.data(),
                                 pertition_mapping_total);
        ispc::parition_as_gather(batch_positions_x.cast<int>().data(),
                                 partition.data(),
                                 partition_buffer_data.data(),
                                 pertition_mapping_total);
        ispc::parition_as_gather(batch_positions_y.cast<int>().data(),
                                 partition.data(),
                                 partition_buffer_data.data(),
                                 pertition_mapping_total);
        ispc::parition_as_gather(batch_positions_z.cast<int>().data(),
                                 partition.data(),
                                 partition_buffer_data.data(),
                                 pertition_mapping_total);


        BLI_assert(partition_to_check.as_span() == partition.as_span());
      }
    }

    if (!leaf_joint) {
      if (all_end_on_joint) {
        continue;
      }

      depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
      joint_stack.extend_unchecked({joint_i * 2 + 1, joint_i * 2 + 0});
      prefix_to_visit_stack.extend_unchecked({total_to_pass_to_childs, total_to_pass_to_childs});
      continue;
    }

    const IndexRange joint_backet = buckets_offsets[joint_i];

    const int bucket_size = joint_backet.size();
    const int aligned_bucket_size = round_up_for(bucket_size, sse_min_alignment);

    bucket_position_data.reinitialize(aligned_bucket_size * 3);
    for (const int axis_i : IndexRange(3)) {
      bucket_position_data.as_mutable_span().slice(aligned_bucket_size * axis_i, bucket_size).copy_from(src_bucket_position[axis_i].slice(joint_backet));
    }
    const Span<float> bucket_positions_x = bucket_position_data.as_mutable_span().slice(aligned_bucket_size * 0, bucket_size);
    const Span<float> bucket_positions_y = bucket_position_data.as_mutable_span().slice(aligned_bucket_size * 1, bucket_size);
    const Span<float> bucket_positions_z = bucket_position_data.as_mutable_span().slice(aligned_bucket_size * 2, bucket_size);

    constexpr int chunk_size = ispc::FMMConstants::ChunkSize;
    const int chunked_batch_size = round_for(total_to_pass_to_childs, chunk_size);
    
    const Span<float[chunk_size]> chunked_batch_x = batch_positions_x.take_front(chunked_batch_size).cast<float[chunk_size]>();
    const Span<float[chunk_size]> chunked_batch_y = batch_positions_y.take_front(chunked_batch_size).cast<float[chunk_size]>();
    const Span<float[chunk_size]> chunked_batch_z = batch_positions_z.take_front(chunked_batch_size).cast<float[chunk_size]>();

    const Span<float> rest_batch_x = batch_positions_x.take_front(total_to_pass_to_childs).drop_front(chunked_batch_size);
    const Span<float> rest_batch_y = batch_positions_y.take_front(total_to_pass_to_childs).drop_front(chunked_batch_size);
    const Span<float> rest_batch_z = batch_positions_z.take_front(total_to_pass_to_childs).drop_front(chunked_batch_size);
    
    BLI_assert(rest_batch_x.size() < chunk_size);
    
    batch_distances_buffer.reinitialize(bucket_size * (chunked_batch_x.size() * chunk_size + rest_batch_x.size()));
    const int total_chunked_table_size = bucket_size * chunked_batch_x.size() * chunk_size;

#ifndef NDEBUG
    batch_distances_buffer.as_mutable_span().fill(-1.0f);
#endif

    const MutableSpan<float[chunk_size]> chunked_distances = batch_distances_buffer.as_mutable_span().take_front(total_chunked_table_size).cast<float[chunk_size]>();
    const MutableSpan<float> rest_distances = batch_distances_buffer.as_mutable_span().drop_front(total_chunked_table_size);

    BLI_assert(chunked_distances.size() == chunked_batch_x.size() * bucket_size);
    ispc::chunked_squared_distances_table(chunked_batch_x.data(),
                                          chunked_batch_y.data(),
                                          chunked_batch_z.data(),
                                          chunked_batch_x.size(),
                                          bucket_positions_x.data(),
                                          bucket_positions_y.data(),
                                          bucket_positions_z.data(),
                                          bucket_positions_x.size(),
                                          chunked_distances.data());

    BLI_assert(rest_distances.size() == rest_batch_x.size() * bucket_size);
    ispc::squared_distances_table(rest_batch_x.data(),
                                  rest_batch_y.data(),
                                  rest_batch_z.data(),
                                  rest_batch_x.size(),
                                  bucket_positions_x.data(),
                                  bucket_positions_y.data(),
                                  bucket_positions_z.data(),
                                  bucket_positions_x.size(),
                                  rest_distances.data());
    BLI_assert(!batch_distances_buffer.as_span().contains(-1.0f));

    ispc::sqrt_n_add_single(batch_distances_buffer.data(), batch_distances_buffer.size(), offset_value);
    distance_invertion(power_value, batch_distances_buffer.as_mutable_span());

    if (sampler_to_bucket_range.has_value()) {
      const IndexRange range_of_samplers = *sampler_to_bucket_range;
      if (!range_of_samplers.intersect(joint_backet).is_empty()) {
        const Span<int> batch_indices = batch_indices_data.take_front(total_to_pass_to_childs);
        const Span<int[chunk_size]> chunked_batch_indices = batch_indices.take_front(chunked_batch_size).cast<int[chunk_size]>();
        const Span<int> rest_batch_indices = batch_indices.drop_front(chunked_batch_size);

        const int from_bucket_to_sampler_offset = joint_backet.start() - range_of_samplers.start();
        ispc::chunked_zero_if_index_in_range(chunked_batch_indices.data(),
                                             bucket_size,
                                             from_bucket_to_sampler_offset,
                                             chunked_batch_indices.size(),
                                             chunked_distances.data());
        ispc::zero_if_index_in_range(rest_batch_indices.data(),
                                     bucket_size,
                                     from_bucket_to_sampler_offset,
                                     rest_batch_indices.size(),
                                     rest_distances.data());
      }
    }

    for (const int data_i : IndexRange(data_axes_num)) {
      const Span<float> backet_values = src_bucket_value[data_i].slice(joint_backet);
    
      const MutableSpan<float> batch_values = batch_values_data[data_i].take_front(total_to_pass_to_childs);
      const MutableSpan<float[chunk_size]> chunked_batch_values = batch_values.take_front(chunked_batch_size).cast<float[chunk_size]>();
      const MutableSpan<float> rest_batch_values = batch_values.drop_front(chunked_batch_size);
      ispc::chunked_table_product_reduce(chunked_distances.data(),
                                         backet_values.size(),
                                         backet_values.data(),
                                         chunked_batch_values.size(),
                                         chunked_batch_values.data());
    
      ispc::table_product_reduce(rest_distances.data(),
                                 backet_values.size(),
                                 backet_values.data(),
                                 rest_batch_values.size(),
                                 rest_batch_values.data());
    }
  }

  for (const int data_i : IndexRange(data_axes_num)) {
    array_utils::scatter<float, int>(batch_values_data[data_i].as_span(), batch_indices_data.as_span(), dst_buckets_data[data_i]);
  }
}

}  // namespace blender::geometry::fmm
