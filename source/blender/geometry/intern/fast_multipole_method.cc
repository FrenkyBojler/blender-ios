/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

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
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_0_n(values.begin(), values.size()); };
    case 1:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_1_n(values.begin(), values.size()); };
    case 2:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_2_n(values.begin(), values.size()); };
    case 3:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_3_n(values.begin(), values.size()); };
    case 4:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_4_n(values.begin(), values.size()); };
    case 5:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_5_n(values.begin(), values.size()); };
    case 6:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_6_n(values.begin(), values.size()); };
    case 7:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_7_n(values.begin(), values.size()); };
    case 8:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_8_n(values.begin(), values.size()); };
    case 9:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_9_n(values.begin(), values.size()); };
    case 10:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_10_n(values.begin(), values.size()); };
    case 11:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_11_n(values.begin(), values.size()); };
    case 12:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_pow_12_n(values.begin(), values.size()); };
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

template<typename T>
static T dot_product(const Span<T> values, const Span<float> factors)
{
  BLI_assert(values.size() == factors.size());
  T accumulator(0);
  for (const int i : values.index_range()) {
    accumulator += values[i] * factors[i];
  }
  return accumulator;
}

// template<>
// static float dot_product(const Span<float> values, const Span<float> factors)
// {
//   BLI_assert(values.size() == factors.size());
//   return ispc::float_dot_product(values.data(), factors.data(), values.size());
// }
// 
// template<>
// static float3 dot_product(const Span<float3> values, const Span<float> factors)
// {
//   BLI_assert(values.size() == factors.size());
//   BLI_assert(!values.is_empty());
//   float3 total(0);
//   ispc::float3_dot_product(values.cast<float [3]>().data(), factors.data(), values.size(), total);
//   return total;
// }

/*
template<typename T>
static void scatter_mul_add(const Span<float> factors, const T value, const Span<int> indices, MutableSpan<T> data)
{
  BLI_assert(factors.size() == indices.size());

  for (const int i : indices.index_range()) {
    data[indices[i]] += value * factors[i];
  }
}

template<>
static void scatter_mul_add(const Span<float> factors, const float value, const Span<int> indices, MutableSpan<float> data)
{
  BLI_assert(factors.size() == indices.size());

  ispc::float_scatter_mul_add(factors.data(), value, indices.data(), data.data(), factors.size());
}
*/

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
  BLI_assert(!sampler_to_bucket_range.has_value() ||
             sampler_to_bucket_range->size() == dst_buckets_data.size());
  BLI_assert(!sampler_to_bucket_range.has_value() ||
             src_bucket_position.index_range().contains(*sampler_to_bucket_range));

  const FunctionRef<void(int, MutableSpan<float>)> distance_invertion = powered_rcp_for_values(
      power_value);

  Vector<std::pair<int, Vector<int, 0>>, 0> joint_to_batch_samples;
  Vector<std::pair<IndexRange, Vector<int>>, 0> bucket_to_batch_samples;

  Array<Vector<int, 0>, 0> batch_to_joints(sample_position.size());

  akdbh::batch_for_each_to_bottom_skip(
      buckets_offsets,
      total_depth,
      sample_position.index_range(),
      [&](const int joint_index, const int batch_i) -> bool {
        const float joint_min_distance_squared = math::square(
            src_joints_min_distance[joint_index] - offset_value);
        const float sampler_to_joint_distance_squared = math::distance_squared(
            src_joints_centre[joint_index], sample_position[batch_i]);
        return sampler_to_joint_distance_squared <= joint_min_distance_squared;
      },
      [&](const int joint_index, const Span<int> batch_indices) {
        // joint_to_batch_samples.append_as(joint_index, batch_indices);
        for (const int batch_i : batch_indices) {
          batch_to_joints[batch_i].append(joint_index);
        }
      },
      [&](const IndexRange bucket_range, const Span<int> batch_indices) {
        bucket_to_batch_samples.append_as(bucket_range, batch_indices);
      });

  Vector<float, 0> buffer;
  buffer.reserve(dst_buckets_data.size());

  to_static_type(src_joints_value.type(), [&](auto dummy) {
    using T = decltype(dummy);

    const Span<T> typed_src_joints_value = src_joints_value.typed<T>();
    const Span<T> typed_src_bucket_value = src_bucket_value.typed<T>();
    MutableSpan<T> typed_dst_buckets_data = dst_buckets_data.typed<T>();

    Vector<T, 0> joints_buffer;
    
    for (const int batch_i : batch_to_joints.index_range()) {
      const Span<int> batch_joints = batch_to_joints[batch_i];
      buffer.resize(batch_joints.size());
      joints_buffer.resize(batch_joints.size());
      
      const float3 batch_position = sample_position[batch_i];
      for (const int i : batch_joints.index_range()) {
        const int joint_index = batch_joints[i];
        const float3 jooint_position = src_joints_centre[joint_index];
        const float sampler_to_joint_distance_squared = math::distance(batch_position, jooint_position);
        buffer[i] = sampler_to_joint_distance_squared + offset_value;
      }
    
      distance_invertion(power_value, buffer.as_mutable_span());
    
      for (const int i : batch_joints.index_range()) {
        const int joint_index = batch_joints[i];
        joints_buffer[i] = typed_src_joints_value[joint_index];
      }
    
      typed_dst_buckets_data[batch_i] += dot_product<T>(joints_buffer.as_span(), buffer.as_span());
    }

    // for (const auto &[joint_index, batch_samples] : joint_to_batch_samples) {
    //   buffer.resize(batch_samples.size());
    //   const float3 jooint_position = src_joints_centre[joint_index];
    //   for (const int sample_i : batch_samples.index_range()) {
    //     const int sample_index = batch_samples[sample_i];
    //     const float sampler_to_joint_distance_squared = math::distance(
    //         jooint_position, sample_position[sample_index]);
    //     buffer[sample_i] = sampler_to_joint_distance_squared + offset_value;
    //   }
    // 
    //   distance_invertion(power_value, buffer.as_mutable_span());
    //   scatter_mul_add(buffer.as_span(), typed_src_joints_value[joint_index], batch_samples.as_span(), typed_dst_buckets_data);
    //   // for (const int sample_i : batch_samples.index_range()) {
    //   //   const int sample_index = batch_samples[sample_i];
    //   //   typed_dst_buckets_data[sample_index] += typed_src_joints_value[joint_index] *
    //   //                                           buffer[sample_i];
    //   // }
    // }
  });

  to_static_type(src_joints_value.type(), [&](auto dummy) {
    using T = decltype(dummy);

    const Span<T> typed_src_joints_value = src_joints_value.typed<T>();
    const Span<T> typed_src_bucket_value = src_bucket_value.typed<T>();
    MutableSpan<T> typed_dst_buckets_data = dst_buckets_data.typed<T>();

    if (!sampler_to_bucket_range.has_value()) {
      for (const auto &[bucket_range, batch_samples] : bucket_to_batch_samples) {
        buffer.resize(bucket_range.size());
        for (const int sample_index : batch_samples) {
          const float3 position = sample_position[sample_index];
          for (const int bucket_i : bucket_range.index_range()) {
            const float sampler_to_point_distance = math::distance(
                position, src_bucket_position[bucket_range[bucket_i]]);
            buffer[bucket_i] = sampler_to_point_distance + offset_value;
          }

          distance_invertion(power_value, buffer.as_mutable_span());
          typed_dst_buckets_data[sample_index] += dot_product<T>(
              typed_src_bucket_value.slice(bucket_range), buffer);
        }
      }
      return;
    }

    for (const auto &[bucket_range, batch_samples] : bucket_to_batch_samples) {
      buffer.resize(bucket_range.size());
      for (const int sample_index : batch_samples) {
        const float3 position = sample_position[sample_index];
        for (const int bucket_i : bucket_range.index_range()) {
          const float sampler_to_point_distance = math::distance(
              position, src_bucket_position[bucket_range[bucket_i]]);
          buffer[bucket_i] = sampler_to_point_distance + offset_value;
        }

        distance_invertion(power_value, buffer.as_mutable_span());

        if (bucket_range.contains(sampler_to_bucket_range.value()[sample_index])) {
          
          const int sampler_in_bucket_index = sampler_to_bucket_range.value()[sample_index] -
                                              bucket_range.start();
          buffer[sampler_in_bucket_index] = 0.0f;
        }

        typed_dst_buckets_data[sample_index] += dot_product<T>(
            typed_src_bucket_value.slice(bucket_range), buffer);
      }
    }
  });
}

}  // namespace blender::geometry::fmm
