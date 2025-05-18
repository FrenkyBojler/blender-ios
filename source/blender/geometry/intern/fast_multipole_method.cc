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

template<typename T>
static T round_for(const T value, const T round_cell)
{
  return (value / round_cell) * round_cell;
}

template<typename T>
static void more_reordered(MutableSpan<T> data, MutableSpan<T> buffer, const int start, const int new_start)
{
  const IndexRange moved_range = IndexRange::from_begin_end(new_start, start);
  buffer.take_front(moved_range.size()).copy_from(data.as_span().slice(moved_range));
  data.slice(moved_range).copy_from(data.take_back(moved_range.size()));
  data.take_back(moved_range.size()).copy_from(buffer.take_front(moved_range.size()).as_span());
}

void akdbh_accumulate_in(const OffsetIndices<int> buckets_offsets,
                         const int total_depth,
                         const Span<float> src_joints_min_distance,
                         const Span<float3> src_joints_centre,
                         const GSpan src_joints_value,
                         const Span<float3> src_bucket_position,
                         const GSpan src_bucket_value,
                         const int power_value,
                         const float offset_value,
                         const Span<float3> sample_position,
                         GMutableSpan dst_buckets_data,
                         const std::optional<IndexRange> sampler_to_bucket_range)
{
  BLI_assert(buckets_offsets.total_size() == src_bucket_position.size());

  BLI_assert(src_joints_min_distance.size() == src_joints_centre.size());
  BLI_assert(src_joints_min_distance.size() == src_joints_value.size());

  BLI_assert(src_bucket_position.size() == src_bucket_value.size());

  BLI_assert(dst_buckets_data.size() == sample_position.size());
  BLI_assert(!sampler_to_bucket_range.has_value() || sampler_to_bucket_range->size() == dst_buckets_data.size());
  BLI_assert(!sampler_to_bucket_range.has_value() || src_bucket_position.index_range().contains(*sampler_to_bucket_range));

  const FunctionRef<void(int, MutableSpan<float>)> distance_invertion = powered_rcp_for_values(power_value);

  const int batch_size = sample_position.size();

  Array<float, 0, GuardedAlignedAllocator<>> batch_positions_x_buffer(batch_size);
  Array<float, 0, GuardedAlignedAllocator<>> batch_positions_y_buffer(batch_size);
  Array<float, 0, GuardedAlignedAllocator<>> batch_positions_z_buffer(batch_size);
  Array<int, 0, GuardedAlignedAllocator<>> batch_indices_buffer(batch_size);
  Array<Array<float, 0, GuardedAlignedAllocator<>>, 3> batch_values_buffer;

  Array<int, 0, GuardedAlignedAllocator<>> partition_buffer(batch_size);

  Vector<float, 0, GuardedAlignedAllocator<>> batch_distances_buffer(batch_size);

  static_assert(sizeof(int) == sizeof(float));
  static_assert(alignof(int) == alignof(float));
  Array<int, 0, GuardedAlignedAllocator<>> buffer_data(batch_size);

  ispc::split_float3_to_3_float(sample_position.cast<float[3]>().data(),
                                batch_positions_x_buffer.as_mutable_span().data(),
                                batch_positions_y_buffer.as_mutable_span().data(),
                                batch_positions_z_buffer.as_mutable_span().data(),
                                batch_size);

  const CPPType &value_type = src_joints_value.type();
  if (value_type.is<float>()) {
    batch_values_buffer.reinitialize(1);
    batch_values_buffer[0].reinitialize(batch_size);
    batch_values_buffer[0].as_mutable_span().fill(0.0f);
  }
  else {
    BLI_assert(value_type.is<float3>());
    batch_values_buffer.reinitialize(3);
    batch_values_buffer[0].reinitialize(batch_size);
    batch_values_buffer[0].as_mutable_span().fill(0.0f);
    batch_values_buffer[1].reinitialize(batch_size);
    batch_values_buffer[1].as_mutable_span().fill(0.0f);
    batch_values_buffer[2].reinitialize(batch_size);
    batch_values_buffer[2].as_mutable_span().fill(0.0f);
  }

  array_utils::fill_index_range<int>(batch_indices_buffer.as_mutable_span(), 0);

  Vector<int, 32> depth_stack({0});
  Vector<int, 32> joint_stack({0});
  Vector<int, 32> prefix_to_visit_stack({batch_size});

  const MutableSpan<int> buffer = buffer_data.as_mutable_span();

  while (!depth_stack.is_empty()) {
    const int prefix_to_visit = prefix_to_visit_stack.pop_last();
    const int depth_i = depth_stack.pop_last();
    const int joint_i = joint_stack.pop_last();

    const MutableSpan<float> batch_positions_x = batch_positions_x_buffer.as_mutable_span().take_front(prefix_to_visit);
    const MutableSpan<float> batch_positions_y = batch_positions_y_buffer.as_mutable_span().take_front(prefix_to_visit);
    const MutableSpan<float> batch_positions_z = batch_positions_z_buffer.as_mutable_span().take_front(prefix_to_visit);
    const MutableSpan<int> batch_indices = batch_indices_buffer.as_mutable_span().take_front(prefix_to_visit);

    const MutableSpan<int> partition = partition_buffer.as_mutable_span().take_front(prefix_to_visit);

    const IndexRange joints_range = akdbh::joints_range_at_depth(depth_i);

    const int joint_index = joints_range[joint_i];
    const float3 joint_position = src_joints_centre[joint_index];

    batch_distances_buffer.resize(prefix_to_visit);

    ispc::squared_distance_to_n(batch_positions_x.data(),
                                batch_positions_y.data(),
                                batch_positions_z.data(),
                                joint_position,
                                prefix_to_visit,
                                batch_distances_buffer.data());

    const float joint_min_distance = src_joints_min_distance[joint_index];

    const int total_next = ispc::count_float_less_than(
        batch_distances_buffer.data(),
        math::square(joint_min_distance),
        prefix_to_visit);

    if (UNLIKELY(total_next == prefix_to_visit)) {
      if (depth_i < total_depth - 1) {
        depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
        joint_stack.extend_unchecked({joint_i * 2 + 1, joint_i * 2 + 0});
        prefix_to_visit_stack.extend_unchecked({prefix_to_visit, prefix_to_visit});
        continue;
      }

      const IndexRange joint_backet = buckets_offsets[joint_i];

      Array<float, 0, GuardedAlignedAllocator<>> backet_positions_x(joint_backet.size());
      Array<float, 0, GuardedAlignedAllocator<>> backet_positions_y(joint_backet.size());
      Array<float, 0, GuardedAlignedAllocator<>> backet_positions_z(joint_backet.size());

      ispc::split_float3_to_3_float(src_bucket_position.slice(joint_backet).cast<float[3]>().data(),
                                    backet_positions_x.data(),
                                    backet_positions_y.data(),
                                    backet_positions_z.data(),
                                    joint_backet.size());

      batch_distances_buffer.resize(joint_backet.size() * prefix_to_visit);

      constexpr int chunk_size = 16;
      const int total_batch_chunks = prefix_to_visit / chunk_size;
      const int chunked_batch_size = round_for(prefix_to_visit, chunk_size);

      const Span<float[16]> chunked_batch_x = batch_positions_x.take_front(chunked_batch_size).cast<float[16]>();
      const Span<float[16]> chunked_batch_y = batch_positions_y.take_front(chunked_batch_size).cast<float[16]>();
      const Span<float[16]> chunked_batch_z = batch_positions_z.take_front(chunked_batch_size).cast<float[16]>();

      const Span<float> rest_batch_x = batch_positions_x.drop_front(chunked_batch_size);
      const Span<float> rest_batch_y = batch_positions_y.drop_front(chunked_batch_size);
      const Span<float> rest_batch_z = batch_positions_z.drop_front(chunked_batch_size);

      const int total_chunked_table_size = joint_backet.size() * chunked_batch_size;
      const MutableSpan<float[16]> chunked_distances = batch_distances_buffer.as_mutable_span().take_front(total_chunked_table_size).cast<float[16]>();
      const MutableSpan<float> rest_distances = batch_distances_buffer.as_mutable_span().drop_front(total_chunked_table_size);

      ispc::chunked_squared_distances_table(chunked_batch_x.data(),
                                            chunked_batch_y.data(),
                                            chunked_batch_z.data(),
                                            chunked_batch_x.size(),
                                            backet_positions_x.data(),
                                            backet_positions_y.data(),
                                            backet_positions_z.data(),
                                            backet_positions_x.size(),
                                            chunked_distances.data());

      ispc::squared_distances_table(rest_batch_x.data(),
                                    rest_batch_y.data(),
                                    rest_batch_z.data(),
                                    rest_batch_z.size(),
                                    backet_positions_x.data(),
                                    backet_positions_y.data(),
                                    backet_positions_z.data(),
                                    backet_positions_x.size(),
                                    rest_distances.data());

      ispc::sqrt_n_add_single(batch_distances_buffer.data(), batch_distances_buffer.size(), offset_value);
      distance_invertion(power_value, batch_distances_buffer.as_mutable_span());
/*
      if (sampler_to_bucket_range->contains(bucket_item)) {
        ispc::zero_if_in_index_n(batch_indices.data(),
                                 batch_distances_buffer.data(),
                                 bucket_item - sampler_to_bucket_range->start(),
                                 prefix_to_visit);
      }
*/
      Array<float, 0, GuardedAlignedAllocator<>> backet_values_x(joint_backet.size());
      Array<float, 0, GuardedAlignedAllocator<>> backet_values_y(joint_backet.size());
      Array<float, 0, GuardedAlignedAllocator<>> backet_values_z(joint_backet.size());
      if (value_type.is<float>()) {
        backet_values_x.as_mutable_span().copy_from(src_bucket_value.typed<float>().slice(joint_backet));
      } else if (value_type.is<float3>()) {
        ispc::split_float3_to_3_float(src_bucket_value.typed<float3>().slice(joint_backet).cast<float[3]>().data(),
                                      backet_values_x.data(),
                                      backet_values_y.data(),
                                      backet_values_z.data(),
                                      joint_backet.size());
      }

      const std::array<Span<float>, 3> backet_values = {backet_values_x.as_span(), backet_values_y.as_span(), backet_values_z.as_span()};

      for (const int data_i : IndexRange(value_type.is<float>() ? 1 : 3)) {
        const MutableSpan<float> batch_values = batch_values_buffer[data_i].as_mutable_span().take_front(prefix_to_visit);
        const MutableSpan<float[16]> chunked_batch_values = batch_values.take_front(chunked_batch_size).cast<float[16]>();
        const MutableSpan<float> rest_batch_values = batch_values.drop_front(chunked_batch_size);
        ispc::chunked_table_product_reduce(chunked_distances.data(),
                                           backet_values[data_i].size(),
                                           backet_values[data_i].data(),
                                           chunked_batch_values.size(),
                                           chunked_batch_values.data());

        ispc::table_product_reduce(rest_distances.data(),
                                   backet_values[data_i].size(),
                                   backet_values[data_i].data(),
                                   rest_batch_values.size(),
                                   rest_batch_values.data());
      }

      continue;
    }

    int pertition_mapping_total = -1;
    if (total_next > 0) {
      pertition_mapping_total = ispc::predicate_partition_indices_float_cmp(
          partition.data(),
          batch_distances_buffer.data(),
          prefix_to_visit,
          math::square(joint_min_distance),
          total_next);

      ispc::parition_as_gather_front(batch_distances_buffer.as_mutable_span().cast<int>().data(),
                                     partition.data(),
                                     buffer.data(),
                                     pertition_mapping_total,
                                     prefix_to_visit,
                                     total_next);
    }

    ispc::sqrt_n_add_single(batch_distances_buffer.data(), prefix_to_visit - total_next, offset_value);
    distance_invertion(power_value, batch_distances_buffer.as_mutable_span().take_front(prefix_to_visit - total_next));

    if (LIKELY(total_next > 0)) {
      if (value_type.is<float>()) {
        const MutableSpan<float> batch_values = batch_values_buffer[0].as_mutable_span().take_front(prefix_to_visit);
        ispc::parition_as_gather(batch_values.cast<int>().data(),
                                 partition.data(),
                                 buffer.data(),
                                 pertition_mapping_total);
      }
      else {
        MutableSpan<float> batch_values = batch_values_buffer[0].as_mutable_span().take_front(prefix_to_visit);
        ispc::parition_as_gather(batch_values.cast<int>().data(),
                                 partition.data(),
                                 buffer.data(),
                                 pertition_mapping_total);

        batch_values = batch_values_buffer[1].as_mutable_span().take_front(prefix_to_visit);
        ispc::parition_as_gather(batch_values.cast<int>().data(),
                                 partition.data(),
                                 buffer.data(),
                                 pertition_mapping_total);

        batch_values = batch_values_buffer[2].as_mutable_span().take_front(prefix_to_visit);
        ispc::parition_as_gather(batch_values.cast<int>().data(),
                                 partition.data(),
                                 buffer.data(),
                                 pertition_mapping_total);
      }
    }

    if (value_type.is<float>()) {
      const float joint_value = src_joints_value.typed<float>()[joint_index];

      const MutableSpan<float> batch_values = batch_values_buffer[0].as_mutable_span().take_front(prefix_to_visit);
      ispc::mul_n_add_to(batch_values.data() + total_next,
                         batch_distances_buffer.data(),
                         joint_value,
                         prefix_to_visit - total_next);
    }
    else {
      const float3 joint_value = src_joints_value.typed<float3>()[joint_index];

      MutableSpan<float> batch_values = batch_values_buffer[0].as_mutable_span().take_front(prefix_to_visit);
      ispc::mul_n_add_to(batch_values.data() + total_next,
                         batch_distances_buffer.data(),
                         joint_value.x,
                         prefix_to_visit - total_next);

      batch_values = batch_values_buffer[1].as_mutable_span().take_front(prefix_to_visit);
      ispc::mul_n_add_to(batch_values.data() + total_next,
                         batch_distances_buffer.data(),
                         joint_value.y,
                         prefix_to_visit - total_next);

      batch_values = batch_values_buffer[2].as_mutable_span().take_front(prefix_to_visit);
      ispc::mul_n_add_to(batch_values.data() + total_next,
                         batch_distances_buffer.data(),
                         joint_value.z,
                         prefix_to_visit - total_next);
    }

    if (LIKELY(total_next > 0)) {
      ispc::parition_as_gather(batch_indices.cast<int>().data(),
                               partition.data(),
                               buffer.data(),
                               pertition_mapping_total);
      ispc::parition_as_gather(batch_positions_x.cast<int>().data(),
                               partition.data(),
                               buffer.data(),
                               pertition_mapping_total);
      ispc::parition_as_gather(batch_positions_y.cast<int>().data(),
                               partition.data(),
                               buffer.data(),
                               pertition_mapping_total);
      ispc::parition_as_gather(batch_positions_z.cast<int>().data(),
                               partition.data(),
                               buffer.data(),
                               pertition_mapping_total);
    }

    if (UNLIKELY(depth_i == total_depth - 1)) {
      const IndexRange joint_backet = buckets_offsets[joint_i];

      // TODO...

      continue;
    }

    if (UNLIKELY(total_next == 0)) {
      continue;
    }

    depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
    joint_stack.extend_unchecked({joint_i * 2 + 1, joint_i * 2 + 0});
    prefix_to_visit_stack.extend_unchecked({total_next, total_next});
  }

  if (value_type.is<float>()) {
    array_utils::scatter<float, int>(batch_values_buffer[0].as_span(),
                                     batch_indices_buffer.as_span(),
                                     dst_buckets_data.typed<float>());
  }
  else {
    for (const int i : IndexRange(batch_size)) {
      const int index = batch_indices_buffer[i];
      dst_buckets_data.typed<float3>()[index].x = batch_values_buffer[0][i];
      dst_buckets_data.typed<float3>()[index].y = batch_values_buffer[1][i];
      dst_buckets_data.typed<float3>()[index].z = batch_values_buffer[2][i];
    }
  }
}

}  // namespace blender::geometry::fmm
