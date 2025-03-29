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

static FunctionRef<void(int, MutableSpan<float>)> powered_rcp_for_values(const int power_value)
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

static FunctionRef<void(int, MutableSpan<float>)> powered_rcp_for_values_new(const int power_value)
{
  switch (power_value) {
    case 0:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_0_rpow_n(values.begin(), values.size()); };
    case 1:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_1_rpow_n(values.begin(), values.size()); };
    case 2:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_2_rpow_n(values.begin(), values.size()); };
    case 3:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_3_rpow_n(values.begin(), values.size()); };
    case 4:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_4_rpow_n(values.begin(), values.size()); };
    case 5:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_5_rpow_n(values.begin(), values.size()); };
    case 6:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_6_rpow_n(values.begin(), values.size()); };
    case 7:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_7_rpow_n(values.begin(), values.size()); };
    case 8:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_8_rpow_n(values.begin(), values.size()); };
    case 9:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_9_rpow_n(values.begin(), values.size()); };
    case 10:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_10_rpow_n(values.begin(), values.size()); };
    case 11:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_11_rpow_n(values.begin(), values.size()); };
    case 12:
      return [](int /*power_value*/, MutableSpan<float> values) { ispc::fixed_safe_12_rpow_n(values.begin(), values.size()); };
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

template<typename T>
static T gather_dot_product(const Span<T> values, const Span<int> indices, const Span<float> factors)
{
  BLI_assert(indices.size() == factors.size());
  T accumulator(0);
  for (const int i : indices.index_range()) {
    accumulator += values[indices[i]] * factors[i];
  }
  return accumulator;
}

// template<>
// static float gather_dot_product(const Span<float> values, const Span<int> indices, const Span<float> factors)
// {
//   BLI_assert(indices.size() == factors.size());
//   return ispc::float_gather_dot_product(indices.data(), values.data(), factors.data(), factors.size());
// }
// 
// template<>
// static float3 gather_dot_product(const Span<float3> values, const Span<int> indices, const Span<float> factors)
// {
//   BLI_assert(indices.size() == factors.size());
//   float3 total(0);
//   ispc::float3_gather_dot_product(indices.data(), values.cast<float [3]>().data(), factors.data(), factors.size(), total);
//   return total;
// }

void gather_distances(const Span<float3> positions, const Span<int> indices, const float3 target, const float offset_value, MutableSpan<float> distances)
{
  BLI_assert(indices.size() == distances.size());
  for (const int i : distances.index_range()) {
    distances[i] = math::distance(positions[indices[i]], target) + offset_value;
  }
}

void gather_distances_new(const Span<float3> positions, const Span<int> indices, const float3 target, const float offset_value, MutableSpan<float> distances)
{
  BLI_assert(indices.size() == distances.size());
  ispc::gather_distances(indices.data(),
                         positions.cast<float [3]>().data(),
                         target,
                         distances.size(),
                         distances.data(),
                         offset_value);
}

// void akdbh_accumulate_in(const OffsetIndices<int> buckets_offsets,
//                          const int total_depth,
//                          const Span<float> src_joints_min_distance,
//                          const Span<float3> src_joints_centre,
//                          const GSpan src_joints_value,
//                          const Span<float3> src_bucket_position,
//                          const GSpan src_bucket_value,
//                          const int power_value,
//                          const float offset_value,
//                          const Span<float3> sample_position,
//                          GMutableSpan dst_buckets_data,
//                          const std::optional<IndexRange> sampler_to_bucket_range)
// {
//   BLI_assert(buckets_offsets.total_size() == src_bucket_position.size());
// 
//   BLI_assert(src_joints_min_distance.size() == src_joints_centre.size());
//   BLI_assert(src_joints_min_distance.size() == src_joints_value.size());
// 
//   BLI_assert(src_bucket_position.size() == src_bucket_value.size());
// 
//   BLI_assert(dst_buckets_data.size() == sample_position.size());
//   BLI_assert(!sampler_to_bucket_range.has_value() ||
//              sampler_to_bucket_range->size() == dst_buckets_data.size());
//   BLI_assert(!sampler_to_bucket_range.has_value() ||
//              src_bucket_position.index_range().contains(*sampler_to_bucket_range));
// 
//   const FunctionRef<void(int, MutableSpan<float>)> distance_invertion = powered_rcp_for_values(
//       power_value);
// 
//   Vector<std::pair<int, Vector<int, 0>>, 0> joint_to_batch_samples;
//   Vector<std::pair<IndexRange, Vector<int>>, 0> bucket_to_batch_samples;
// 
//   // Array<Vector<int, 0>, 0> batch_to_joints(sample_position.size());
//   // Array<Vector<int, 0>, 0> batch_to_buckets(sample_position.size());
// 
//   // BLI_assert(sample_position.size() < std::numeric_limits<int16_t>::max());
// 
//   std::stringstream log_stream;
// 
//   {
//     // SCOPED_TIMER_AVERAGED("  batch_for_each_to_bottom_skip");
//     akdbh::batch_for_each_to_bottom_skip(
//         buckets_offsets,
//         total_depth,
//         sample_position.index_range(),
//         [&](const int joint_index, const int batch_i) -> bool {
//           const float joint_min_distance_squared = math::square(src_joints_min_distance[joint_index] - offset_value);
//           const float sampler_to_joint_distance_squared = math::distance_squared(src_joints_centre[joint_index], sample_position[batch_i]);
//           return sampler_to_joint_distance_squared <= joint_min_distance_squared;
//         },
//         [&](const int joint_index, const Span<int> batch_indices) {
//           joint_to_batch_samples.append_as(joint_index, batch_indices);
//           // for (const int batch_i : batch_indices) {
//           //   batch_to_joints[batch_i].append_as(joint_index);
//           // }
//         },
//         [&](const IndexRange bucket_range, const Span<int> batch_indices) {
//           bucket_to_batch_samples.append_as(bucket_range, batch_indices);
//           // for (const int batch_i : batch_indices) {
//           //   Vector<int, 0> &batch_buckets = batch_to_buckets[batch_i];
//           //   batch_buckets.resize(batch_buckets.size() + bucket_range.size());
//           //   std::iota(batch_buckets.end() - bucket_range.size(), batch_buckets.end(), bucket_range.first());
//           // }
//         });
//   }
// 
//   BLI_assert(std::all_of(bucket_to_batch_samples.begin(), bucket_to_batch_samples.end() - 1, [&](const auto &item) {
//     const int index = std::distance(bucket_to_batch_samples.as_span().data(), &item);
//     return item.first.last() < bucket_to_batch_samples[index + 1].first.start();
//   }));
// 
//   /*
//   log_stream << "batch_to_joints.size: " << batch_to_joints.size() << ";\n";
//   int64_t total = 0;
//   for (const auto &item : batch_to_joints) {
//     total += item.size();
//   }
// 
//   log_stream << "total in batch_to_joints:" << total << ";\n";
// 
//   log_stream << "bucket_to_batch_samples.size: " << bucket_to_batch_samples.size() << ";\n";
// 
//   int64_t total_ranges = 0;
//   int64_t total_indices = 0;
//   for (const auto &item : bucket_to_batch_samples) {
//     total_ranges += item.first.size();
//     total_indices += item.second.size();
//   }
// 
//   log_stream << "total_ranges in bucket_to_batch_samples:" << total_ranges << ";\n";
//   log_stream << "total_indices in bucket_to_batch_samples:" << total_indices << ";\n";
// 
//   std::cout << log_stream.str() << ";\n";
//   */
// 
//   Vector<float, 0, GuardedAlignedAllocator<>> buffer;
// 
//   to_static_type(src_joints_value.type(), [&](auto dummy) {
//     // SCOPED_TIMER_AVERAGED("  batch_to_joints");
//     using T = decltype(dummy);
// 
//     const Span<T> typed_src_joints_value = src_joints_value.typed<T>();
//     const Span<T> typed_src_bucket_value = src_bucket_value.typed<T>();
//     MutableSpan<T> typed_dst_buckets_data = dst_buckets_data.typed<T>();
// 
//     // Vector<T, 0> joints_buffer;
//     
//     /*
//     for (const int batch_i : batch_to_joints.index_range()) {
//       const Span<int> batch_joints = batch_to_joints[batch_i];
//       buffer.resize(batch_joints.size());
//       // joints_buffer.resize(batch_joints.size());
//       
//       const float3 batch_position = sample_position[batch_i];
//     
//       gather_distances(src_joints_centre, batch_joints, batch_position, offset_value, buffer);
//     
//       // for (const int i : batch_joints.index_range()) {
//       //   const int joint_index = batch_joints[i];
//       //   const float3 jooint_position = src_joints_centre[joint_index];
//       //   const float sampler_to_joint_distance_squared = math::distance(batch_position, jooint_position);
//       //   buffer[i] = sampler_to_joint_distance_squared + offset_value;
//       // }
//     
//       distance_invertion(power_value, buffer.as_mutable_span());
//     
//       // for (const int i : batch_joints.index_range()) {
//       //   const int joint_index = batch_joints[i];
//       //   joints_buffer[i] = typed_src_joints_value[joint_index];
//       // }
//     
//       typed_dst_buckets_data[batch_i] += gather_dot_product<T>(typed_src_joints_value, batch_joints, buffer.as_span());
//     
//       // typed_dst_buckets_data[batch_i] += dot_product<T>(joints_buffer.as_span(), buffer.as_span());
//     }
//     */
// 
//     buffer.resize(std::accumulate(joint_to_batch_samples.begin(), joint_to_batch_samples.end(), 0, [&](const int size, const auto &item) {
//       return size + item.second.size();
//     }));
// 
//     int offset_iter = 0;
//     for (const auto &[joint_index, batch_samples] : joint_to_batch_samples) {
//       MutableSpan<float> buffer_section = buffer.as_mutable_span().slice(offset_iter, batch_samples.size());
//       offset_iter += batch_samples.size();
// 
//       const float3 jooint_position = src_joints_centre[joint_index];
//       for (const int sample_i : batch_samples.index_range()) {
//         const int sample_index = batch_samples[sample_i];
//         const float sampler_to_joint_distance_squared = math::distance(jooint_position, sample_position[sample_index]);
//         buffer_section[sample_i] = sampler_to_joint_distance_squared + offset_value;
//       }
//     }
// 
//     distance_invertion(power_value, buffer.as_mutable_span());
// 
//     offset_iter = 0;
//     for (const auto &[joint_index, batch_samples] : joint_to_batch_samples) {
//       const Span<float> buffer_section = buffer.as_span().slice(offset_iter, batch_samples.size());
//       offset_iter += batch_samples.size();
// 
//       // scatter_mul_add(buffer_section.as_span(), typed_src_joints_value[joint_index], batch_samples.as_span(), typed_dst_buckets_data);
//       for (const int sample_i : batch_samples.index_range()) {
//         const int sample_index = batch_samples[sample_i];
//         typed_dst_buckets_data[sample_index] += typed_src_joints_value[joint_index] * buffer_section[sample_i];
//       }
//     }
//   });
// 
//   to_static_type(src_joints_value.type(), [&](auto dummy) {
//     // SCOPED_TIMER_AVERAGED("  bucket_to_batch_samples");
//     using T = decltype(dummy);
// 
//     const Span<T> typed_src_joints_value = src_joints_value.typed<T>();
//     const Span<T> typed_src_bucket_value = src_bucket_value.typed<T>();
//     MutableSpan<T> typed_dst_buckets_data = dst_buckets_data.typed<T>();
// 
//     if (!sampler_to_bucket_range.has_value()) {
//       for (const auto &[bucket_range, batch_samples] : bucket_to_batch_samples) {
//         buffer.resize(bucket_range.size());
//         for (const int sample_index : batch_samples) {
//           const float3 position = sample_position[sample_index];
//   
//           ispc::distances(const_cast<float (*)[3] >(src_bucket_position.slice(bucket_range).cast<float [3]>().data()),
//                           position,
//                           buffer.size(),
//                           buffer.data(),
//                           offset_value);
//   
//           // for (const int bucket_i : bucket_range.index_range()) {
//           //   const float sampler_to_point_distance = math::distance(position, src_bucket_position[bucket_range[bucket_i]]);
//           //   buffer[bucket_i] = sampler_to_point_distance + offset_value;
//           // }
//   
//           distance_invertion(power_value, buffer.as_mutable_span());
//           typed_dst_buckets_data[sample_index] += dot_product<T>(
//               typed_src_bucket_value.slice(bucket_range), buffer);
//         }
//       }
//       return;
//     }
// 
// /*
//     Vector<T, 0> joints_buffer;
//     for (const int batch_i : batch_to_buckets.index_range()) {
//       const Span<int> buckets_indices = batch_to_buckets[batch_i];
//       buffer.resize(buckets_indices.size());
//       // joints_buffer.resize(buckets_indices.size());
//       
//       const float3 batch_position = sample_position[batch_i];
//     
//       gather_distances(src_bucket_position, buckets_indices, batch_position, offset_value, buffer);
//     
//       // for (const int bucket_i : bucket_range.index_range()) {
//       //   const float sampler_to_point_distance = math::distance(
//       //       position, src_bucket_position[bucket_range[bucket_i]]);
//       //   buffer[bucket_i] = sampler_to_point_distance + offset_value;
//       // }
//     
//       distance_invertion(power_value, buffer.as_mutable_span());
//       
//       // for (const int i : buckets_indices.index_range()) {
//       //   const int bucket_index = buckets_indices[i];
//       //   joints_buffer[i] = typed_src_bucket_value[bucket_index];
//       // }
//     
//       typed_dst_buckets_data[batch_i] += gather_dot_product<T>(typed_src_bucket_value, buckets_indices, buffer.as_span());
//       //typed_dst_buckets_data[batch_i] += dot_product<T>(joints_buffer.as_span(), buffer.as_span());
//     }
// */
// 
//     buffer.resize(std::accumulate(bucket_to_batch_samples.begin(), bucket_to_batch_samples.end(), 0, [&](const int size, const auto &item) {
//       return size + item.first.size() * item.second.size();
//     }));
// 
// // int more_than_4 = 0;
// // int more_than_8 = 0;
// // int more_than_16 = 0;
// // int total_sum = 0;
// 
//     int offset_iter = 0;
//     for (const auto &[bucket_range, batch_samples] : bucket_to_batch_samples) {
// 
//       for (const int sample_index : batch_samples) {
//         MutableSpan<float> buffer_section = buffer.as_mutable_span().slice(offset_iter, bucket_range.size());
//         offset_iter += bucket_range.size();
// 
// /*
//         total_sum++;
//         if (bucket_range.size() >= 4) {
//           more_than_4++;
//         }
//         if (bucket_range.size() >= 8) {
//           more_than_8++;
//         }
//         if (bucket_range.size() >= 16) {
//           more_than_16++;
//         }
// */
// 
//         const float3 position = sample_position[sample_index];
// 
//         ispc::distances(const_cast<float (*)[3] >(src_bucket_position.slice(bucket_range).cast<float [3]>().data()),
//                         position,
//                         buffer_section.size(),
//                         buffer_section.data(),
//                         offset_value);
//       }
//     }
// 
//     offset_iter = 0;
//     for (const auto &[bucket_range, batch_samples] : bucket_to_batch_samples) {
// 
//       for (const int sample_index : batch_samples) {
//         MutableSpan<float> buffer_section = buffer.as_mutable_span().slice(offset_iter, bucket_range.size());
//         offset_iter += bucket_range.size();
// 
//         if (bucket_range.contains(sampler_to_bucket_range.value()[sample_index])) {
//           const int sampler_in_bucket_index = sampler_to_bucket_range.value()[sample_index] -
//                                               bucket_range.start();
//           buffer_section[sampler_in_bucket_index] = 0.0f;
//         }
//       }
//     }
// 
//     // printf("Total: %d. 4: %d, 8: %d, 16: %d;\n", total_sum, more_than_4, more_than_8, more_than_16);
// 
//     distance_invertion(power_value, buffer.as_mutable_span());
// 
//     offset_iter = 0;
//     for (const auto &[bucket_range, batch_samples] : bucket_to_batch_samples) {
//       for (const int sample_index : batch_samples) {
//         const Span<float> buffer_section = buffer.as_span().slice(offset_iter, bucket_range.size());
//         offset_iter += bucket_range.size();
//         typed_dst_buckets_data[sample_index] += dot_product<T>(typed_src_bucket_value.slice(bucket_range), buffer_section);
//       }
//     }
//   });
// }

// inline int64_t popcount(const bits::BoundedBitSpan data)
// {
//   int count = 0;
//   bits::foreach_1_index(data, [&](const int /*i*/) {
//     count++;
//   });
//   return count;
// 
//   // int64_t count = 0;
//   // for (const int i : IndexRange(data.full_ints_num()).drop_back(1)) {
//   //   count += count_bits_uint64(data.data()[i]);
//   // }
//   // return count + data.data()[data.full_ints_num() - 1] & data.final_bits_num();
// }
// 
// void akdbh_accumulate_in(const OffsetIndices<int> buckets_offsets,
//                          const int total_depth,
//                          const Span<float> src_joints_min_distance,
//                          const Span<float3> src_joints_centre,
//                          const GSpan src_joints_value,
//                          const Span<float3> src_bucket_position,
//                          const GSpan src_bucket_value,
//                          const int power_value,
//                          const float offset_value,
//                          const Span<float3> sample_position,
//                          GMutableSpan dst_buckets_data,
//                          const std::optional<IndexRange> sampler_to_bucket_range)
// {
//   BLI_assert(buckets_offsets.total_size() == src_bucket_position.size());
// 
//   BLI_assert(src_joints_min_distance.size() == src_joints_centre.size());
//   BLI_assert(src_joints_min_distance.size() == src_joints_value.size());
// 
//   BLI_assert(src_bucket_position.size() == src_bucket_value.size());
// 
//   BLI_assert(dst_buckets_data.size() == sample_position.size());
//   BLI_assert(!sampler_to_bucket_range.has_value() || sampler_to_bucket_range->size() == dst_buckets_data.size());
//   BLI_assert(!sampler_to_bucket_range.has_value() || src_bucket_position.index_range().contains(*sampler_to_bucket_range));
// 
//   const FunctionRef<void(int, MutableSpan<float>)> distance_invertion = powered_rcp_for_values(power_value);
// 
//   Vector<std::pair<int, BitVector<0>>, 0> joint_to_batch_samples;
//   Vector<std::pair<IndexRange, BitVector<0>>, 0> bucket_to_batch_samples;
// 
//   std::stringstream log_stream;
// 
//   {
//     // SCOPED_TIMER_AVERAGED("  batch_for_each_to_bottom_skip");
//     akdbh::batch_for_each_to_bottom_skip_(
//         buckets_offsets,
//         total_depth,
//         sample_position.index_range(),
//         [&](const int joint_index, const int batch_i) -> bool {
//           const float joint_min_distance_squared = math::square(src_joints_min_distance[joint_index] - offset_value);
//           const float sampler_to_joint_distance_squared = math::distance_squared(src_joints_centre[joint_index], sample_position[batch_i]);
//           return sampler_to_joint_distance_squared <= joint_min_distance_squared;
//         },
//         [&](const int joint_index, const bits::BoundedBitSpan batch_bits) {
//           if (bits::any_bit_set(batch_bits)) {
//             joint_to_batch_samples.append_as(joint_index, batch_bits);
//           }
//         },
//         [&](const IndexRange bucket_range, const bits::BoundedBitSpan batch_bits) {
//           bucket_to_batch_samples.append_as(bucket_range, batch_bits);
//         });
//   }
// 
//   BLI_assert(bucket_to_batch_samples.is_empty() || std::all_of(bucket_to_batch_samples.begin(), bucket_to_batch_samples.end() - 1, [&](const auto &item) {
//     const int index = std::distance(bucket_to_batch_samples.as_span().data(), &item);
//     return item.first.last() < bucket_to_batch_samples[index + 1].first.start();
//   }));
// 
//   Vector<float, 0, GuardedAlignedAllocator<>> buffer;
// 
//   to_static_type(src_joints_value.type(), [&](auto dummy) {
//     // SCOPED_TIMER_AVERAGED("  batch_to_joints");
//     using T = decltype(dummy);
// 
//     const Span<T> typed_src_joints_value = src_joints_value.typed<T>();
//     const Span<T> typed_src_bucket_value = src_bucket_value.typed<T>();
//     MutableSpan<T> typed_dst_buckets_data = dst_buckets_data.typed<T>();
// 
//     Array<int> offsets_data(joint_to_batch_samples.size() + 1);
//     std::transform(joint_to_batch_samples.begin(), joint_to_batch_samples.end(), offsets_data.begin(), [&](const auto &item) {
//       return popcount(bits::to_best_bit_span(item.second));
//     });
// 
//     const OffsetIndices<int> offsets = offset_indices::accumulate_counts_to_offsets(offsets_data);
// 
//     buffer.resize(offsets.total_size());
// 
//     for (const int index : joint_to_batch_samples.index_range()) {
//       const auto &[joint_index, batch_samples] = joint_to_batch_samples[index];
//       MutableSpan<float> buffer_section = buffer.as_mutable_span().slice(offsets[index]);
// 
//       const float3 jooint_position = src_joints_centre[joint_index];
// 
//       int index_iter = 0;
//       bits::foreach_1_index(bits::to_best_bit_span(batch_samples), [&](const int sample_index) {
//         const float sampler_to_joint_distance_squared = math::distance(jooint_position, sample_position[sample_index]);
//         buffer_section[index_iter] = sampler_to_joint_distance_squared + offset_value;
//         index_iter++;
//       });
//     }
// 
//     distance_invertion(power_value, buffer.as_mutable_span());
// 
//     for (const int index : joint_to_batch_samples.index_range()) {
//       const auto &[joint_index, batch_samples] = joint_to_batch_samples[index];
//       const Span<float> buffer_section = buffer.as_span().slice(offsets[index]);
// 
//       int index_iter = 0;
//       bits::foreach_1_index(bits::to_best_bit_span(batch_samples), [&](const int sample_index) {
//         typed_dst_buckets_data[sample_index] += typed_src_joints_value[joint_index] * buffer_section[index_iter];
//         index_iter++;
//       });
//     }
//   });
// 
//   to_static_type(src_joints_value.type(), [&](auto dummy) {
//     // SCOPED_TIMER_AVERAGED("  bucket_to_batch_samples");
//     using T = decltype(dummy);
// 
//     const Span<T> typed_src_joints_value = src_joints_value.typed<T>();
//     const Span<T> typed_src_bucket_value = src_bucket_value.typed<T>();
//     MutableSpan<T> typed_dst_buckets_data = dst_buckets_data.typed<T>();
// 
//     Array<int> offsets_data(bucket_to_batch_samples.size() + 1);
//     std::transform(bucket_to_batch_samples.begin(), bucket_to_batch_samples.end(), offsets_data.begin(), [&](const auto &item) {
//       return item.first.size() * popcount(bits::to_best_bit_span(item.second));
//     });
// 
//     const OffsetIndices<int> offsets = offset_indices::accumulate_counts_to_offsets(offsets_data);
// 
//     buffer.resize(offsets.total_size());
// 
//     for (const int index : bucket_to_batch_samples.index_range()) {
//       const auto &[bucket_range, batch_samples] = bucket_to_batch_samples[index];
// 
//       const int buffer_step_size = bucket_range.size();
//       int offset_iter = 0;
//       bits::foreach_1_index(bits::to_best_bit_span(batch_samples), [&](const int sample_index) {
//         MutableSpan<float> buffer_section = buffer.as_mutable_span().slice(offsets[index]).slice(offset_iter, buffer_step_size);
//         offset_iter += buffer_step_size;
// 
//         const float3 position = sample_position[sample_index];
// 
//         ispc::distances(const_cast<float (*)[3] >(src_bucket_position.slice(bucket_range).cast<float [3]>().data()),
//                         position,
//                         buffer_section.size(),
//                         buffer_section.data(),
//                         offset_value);
//       });
//     }
// 
//     for (const int index : bucket_to_batch_samples.index_range()) {
//       const auto &[bucket_range, batch_samples] = bucket_to_batch_samples[index];
// 
//       const int buffer_step_size = bucket_range.size();
//       int offset_iter = 0;
//       bits::foreach_1_index(bits::to_best_bit_span(batch_samples), [&](const int sample_index) {
//         MutableSpan<float> buffer_section = buffer.as_mutable_span().slice(offsets[index]).slice(offset_iter, buffer_step_size);
//         offset_iter += buffer_step_size;
// 
//         if (bucket_range.contains(sampler_to_bucket_range.value()[sample_index])) {
//           const int sampler_in_bucket_index = sampler_to_bucket_range.value()[sample_index] - bucket_range.start();
//           buffer_section[sampler_in_bucket_index] = 0.0f;
//         }
//       });
//     }
// 
//     distance_invertion(power_value, buffer.as_mutable_span());
// 
//     for (const int index : bucket_to_batch_samples.index_range()) {
//       const auto &[bucket_range, batch_samples] = bucket_to_batch_samples[index];
// 
//       const int buffer_step_size = bucket_range.size();
//       int offset_iter = 0;
//       bits::foreach_1_index(bits::to_best_bit_span(batch_samples), [&](const int sample_index) {
//         const Span<float> buffer_section = buffer.as_span().slice(offsets[index]).slice(offset_iter, buffer_step_size);
//         offset_iter += buffer_step_size;
//         typed_dst_buckets_data[sample_index] += dot_product<T>(typed_src_bucket_value.slice(bucket_range), buffer_section);
//       });
//     }
//   });
// }

template<class T, class TPredicate>
static int64_t index_swap_partition(const MutableSpan<T> values, const TPredicate &predicate)
{
  const int64_t prefix_size = std::count_if(values.index_range().begin(), values.index_range().end(), predicate);

  int64_t false_iter = prefix_size;
  for (const int front_index : IndexRange(prefix_size)) {
    if (predicate(front_index)) {
      continue;
    }
    for (const int back_index : IndexRange::from_begin_end(false_iter, values.size())) {
      if (predicate(back_index)) {
        std::swap(values[front_index], values[back_index]);
        false_iter = back_index + 1;
      }
    }
  }

  return prefix_size;
}

template<typename T, typename Func>
static int64_t partition_i_buffer(const MutableSpan<T> buffer, const MutableSpan<T> data, const Func &func)
{
  
  // return std::distance(data.data(), std::partition(data.begin(), data.end(), [&](const auto &item) {
  //   return func(std::distance(data.as_span().data(), &item));
  // }));
  
  BLI_assert(buffer.size() == data.size());
  T *__restrict buffer_ptr = buffer.data();
  const T *__restrict data_ptr = data.data();
  
  for (int64_t index = 0; index < data.size(); index++) {
    *buffer_ptr = *data_ptr;
    buffer_ptr += bool(func(index));
    data_ptr++;
  }

  const int64_t total_front = std::distance(buffer.data(), buffer_ptr);

  if (ELEM(total_front, 0, data.size())) {
    return total_front;
  }

  data_ptr = data.data();
  for (int64_t index = 0; index < data.size(); index++) {
    *buffer_ptr = *data_ptr;
    buffer_ptr += !bool(func(index));
    data_ptr++;
  }

  std::copy(buffer.begin(), buffer.end(), data.begin());
  
  return total_front;
}

/*
template<typename Func>
static int64_t partition_i(const MutableSpan<int> data, const Func &func)
{
  int *__restrict data_ptr = data.data();
  
  for (int64_t index = 0; index < data.size(); index++) {
    *data_ptr = index;
    data_ptr += bool(func(index));
  }

  const int64_t total_front = std::distance(data.data(), data_ptr);

  if (total_front == data.size()) {
    return total_front;
  }

  if (total_front == 0) {
    array_utils::fill_index_range<int>(data);
    return total_front;
  }

  for (int64_t index = 0; index < data.size(); index++) {
    *data_ptr = index;
    data_ptr += !bool(func(index));
  }

  return total_front;
}
*/

template<typename T, typename Func>
static int64_t copy_if_i(const MutableSpan<T> data, const Func &func)
{
  T *src_ptr = data.data();
  T *dst_ptr = data.data();
  
  for (int64_t index = 0; index < data.size(); index++) {
    *dst_ptr = *src_ptr;
    dst_ptr += bool(func(index));
    src_ptr++;
  }

  return std::distance(data.data(), dst_ptr);
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

  {
    const int batch_size = sample_position.size();

    Array<float, 0, GuardedAlignedAllocator<>> batch_positions_x_buffer(batch_size);
    Array<float, 0, GuardedAlignedAllocator<>> batch_positions_y_buffer(batch_size);
    Array<float, 0, GuardedAlignedAllocator<>> batch_positions_z_buffer(batch_size);
    Array<int, 0, GuardedAlignedAllocator<>> batch_indices_buffer(batch_size);
    Array<float, 0, GuardedAlignedAllocator<>> batch_distances_buffer(batch_size);
    Array<Array<float, 0, GuardedAlignedAllocator<>>, 3> batch_values_buffer;

    Array<int, 0, GuardedAlignedAllocator<>> partition_buffer(batch_size);

    static_assert(sizeof(int) == sizeof(float));
    static_assert(alignof(int) == alignof(float));
    Array<int, 0, GuardedAlignedAllocator<>> buffer_data(batch_size);

    const CPPType &value_type = src_joints_value.type();

    // for (const int i : IndexRange(batch_size)) {
    //   batch_positions_x_buffer[i] = sample_position[i].x;
    //   batch_positions_y_buffer[i] = sample_position[i].y;
    //   batch_positions_z_buffer[i] = sample_position[i].z;
    // }

    ispc::split_float3_to_3_float(sample_position.cast<float [3]>().data(),
                                  batch_positions_x_buffer.as_mutable_span().data(),
                                  batch_positions_y_buffer.as_mutable_span().data(),
                                  batch_positions_z_buffer.as_mutable_span().data(),
                                  batch_size);

    if (value_type.is<float>()) {
      batch_values_buffer.reinitialize(1);
      batch_values_buffer[0].reinitialize(batch_size);
      batch_values_buffer[0].as_mutable_span().fill(0.0f);
    } else {
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

    // Vector<int, 32> axis_rotate_stack({0});

    const MutableSpan<int> buffer = buffer_data.as_mutable_span();

    while (!depth_stack.is_empty()) {
      const int prefix_to_visit = prefix_to_visit_stack.pop_last();
      const int depth_i = depth_stack.pop_last();
      const int joint_i = joint_stack.pop_last();

      const MutableSpan<float> batch_positions_x = batch_positions_x_buffer.as_mutable_span().take_front(prefix_to_visit);
      const MutableSpan<float> batch_positions_y = batch_positions_y_buffer.as_mutable_span().take_front(prefix_to_visit);
      const MutableSpan<float> batch_positions_z = batch_positions_z_buffer.as_mutable_span().take_front(prefix_to_visit);

      const MutableSpan<float> batch_distances = batch_distances_buffer.as_mutable_span().take_front(prefix_to_visit);
      const MutableSpan<int> batch_indices = batch_indices_buffer.as_mutable_span().take_front(prefix_to_visit);

      const MutableSpan<int> partition = partition_buffer.as_mutable_span().take_front(prefix_to_visit);

      const IndexRange joints_range = akdbh::joints_range_at_depth(depth_i);

      const int joint_index = joints_range[joint_i];
      const float3 joint_position = src_joints_centre[joint_index];

      ispc::distances_split(batch_positions_x.data(),
                            batch_positions_y.data(),
                            batch_positions_z.data(),
                            joint_position,
                            batch_distances.size(),
                            batch_distances.data(),
                            offset_value);

      const float joint_min_distance = src_joints_min_distance[joint_index];
      
      const int total_next = ispc::float_more_then_single_count(batch_distances.data(), joint_min_distance, prefix_to_visit);

      if (UNLIKELY(total_next == prefix_to_visit)) {
        if (depth_i == total_depth - 1) {
          continue;
        }
        depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
        joint_stack.extend_unchecked({joint_i * 2 + 1, joint_i * 2 + 0});
        prefix_to_visit_stack.extend_unchecked({prefix_to_visit, prefix_to_visit});
        continue;
      }

      // ispc::IndicesStruct front_indices;
      // ispc::IndicesStruct back_indices;
      // const int total_next = ispc::float_compare_n_indices_segmented(batch_distances.data(),
      //                                                                joint_min_distance,
      //                                                                &front_indices,
      //                                                                &back_indices,
      //                                                                partition.data(),
      //                                                                buffer.data(),
      //                                                                prefix_to_visit);

      // const int total_next = ispc::predicate_indices_float_cmp(partition.data(), batch_distances.data(), prefix_to_visit, joint_min_distance);

      int pertition_mapping_total = -1;
      if (total_next > 0) {
        pertition_mapping_total = ispc::predicate_partition_indices_float_cmp(partition.data(),
                                                                              batch_distances.data(),
                                                                              prefix_to_visit,
                                                                              joint_min_distance,
                                                                              total_next);
        // ispc::predicate_revers_indices_float_cmp(partition.data(), batch_distances.data(), prefix_to_visit, joint_min_distance);
      }

      // const int total_next = partition_i(partition, [&](const int i) {
      //   return batch_distances[i] < joint_min_distance;
      // });

      // if (UNLIKELY(total_next == prefix_to_visit)) {
      //   if (depth_i == total_depth - 1) {
      //     continue;
      //   }
      //   depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
      //   joint_stack.extend_unchecked({joint_i * 2 + 1, joint_i * 2 + 0});
      //   prefix_to_visit_stack.extend_unchecked({total_next, total_next});
      //   continue;
      // }

      // ispc::zip_if_larger_or_equal(batch_distances.cast<int>().data(), batch_distances.data(), prefix_to_visit, joint_min_distance);
      if (total_next > 0) {
        // ispc::parition_as_gather_front_only(batch_distances.cast<int>().data(), partition.data(), buffer.data(), pertition_mapping_total);
        ispc::parition_as_gather_back(batch_distances.cast<int>().data(), partition.data(), buffer.data(), pertition_mapping_total);
      }
      
      distance_invertion(power_value, batch_distances.drop_back(total_next));

      if (LIKELY(total_next > 0)) {
        if (value_type.is<float>()) {
          const MutableSpan<float> batch_values = batch_values_buffer[0].as_mutable_span().take_front(prefix_to_visit);
          // ispc::gather_ints_buffer(batch_values.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
          // ispc::scatter_ints_buffer(batch_values.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
          ispc::parition_as_gather(batch_values.cast<int>().data(), partition.data(), buffer.data(), pertition_mapping_total);
          
          // ispc::gather_ints_buffer_segmented(batch_values.cast<int>().data(),
          //                                    partition.data(),
          //                                    buffer.data(),
          //                                    prefix_to_visit,
          //                                    &front_indices,
          //                                    &back_indices);
          
        } else {
          MutableSpan<float> batch_values = batch_values_buffer[0].as_mutable_span().take_front(prefix_to_visit);
          // ispc::gather_ints_buffer(batch_values.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
          // ispc::scatter_ints_buffer(batch_values.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
          ispc::parition_as_gather(batch_values.cast<int>().data(), partition.data(), buffer.data(), pertition_mapping_total);
          
          // ispc::gather_ints_buffer_segmented(batch_values.cast<int>().data(),
          //                                    partition.data(),
          //                                    buffer.data(),
          //                                    prefix_to_visit,
          //                                    &front_indices,
          //                                    &back_indices);
          
          batch_values = batch_values_buffer[1].as_mutable_span().take_front(prefix_to_visit);
          // ispc::gather_ints_buffer(batch_values.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
          // ispc::scatter_ints_buffer(batch_values.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
          ispc::parition_as_gather(batch_values.cast<int>().data(), partition.data(), buffer.data(), pertition_mapping_total);
          // ispc::gather_ints_buffer_segmented(batch_values.cast<int>().data(),
          //                                    partition.data(),
          //                                    buffer.data(),
          //                                    prefix_to_visit,
          //                                    &front_indices,
          //                                    &back_indices);
          
          batch_values = batch_values_buffer[2].as_mutable_span().take_front(prefix_to_visit);
          // ispc::gather_ints_buffer(batch_values.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
          // ispc::scatter_ints_buffer(batch_values.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
          ispc::parition_as_gather(batch_values.cast<int>().data(), partition.data(), buffer.data(), pertition_mapping_total);
          // ispc::gather_ints_buffer_segmented(batch_values.cast<int>().data(),
          //                                    partition.data(),
          //                                    buffer.data(),
          //                                    prefix_to_visit,
          //                                    &front_indices,
          //                                    &back_indices);
        }
      }

      if (value_type.is<float>()) {
        const float joint_value = src_joints_value.typed<float>()[joint_index];

        const MutableSpan<float> batch_values = batch_values_buffer[0].as_mutable_span().take_front(prefix_to_visit);
        ispc::one_mul_add_n(batch_values.data() + total_next, batch_distances.data(), joint_value, prefix_to_visit - total_next);
      } else {
        const float3 joint_value = src_joints_value.typed<float3>()[joint_index];

        MutableSpan<float> batch_values = batch_values_buffer[0].as_mutable_span().take_front(prefix_to_visit);
        ispc::one_mul_add_n(batch_values.data() + total_next, batch_distances.data(), joint_value.x, prefix_to_visit - total_next);
        
        batch_values = batch_values_buffer[1].as_mutable_span().take_front(prefix_to_visit);
        ispc::one_mul_add_n(batch_values.data() + total_next, batch_distances.data(), joint_value.y, prefix_to_visit - total_next);
        
        batch_values = batch_values_buffer[2].as_mutable_span().take_front(prefix_to_visit);
        ispc::one_mul_add_n(batch_values.data() + total_next, batch_distances.data(), joint_value.z, prefix_to_visit - total_next);
      }

      if (LIKELY(total_next > 0)) {
        // ispc::gather_ints_buffer(batch_indices.data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
        // ispc::scatter_ints_buffer(batch_indices.data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
        ispc::parition_as_gather(batch_indices.cast<int>().data(), partition.data(), buffer.data(), pertition_mapping_total);
        
        // ispc::gather_ints_buffer_segmented(batch_indices.data(),
        //                                    partition.data(),
        //                                    buffer.data(),
        //                                    prefix_to_visit,
        //                                    &front_indices,
        //                                    &back_indices);

        // ispc::gather_ints_buffer(batch_positions_x.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
        // ispc::scatter_ints_buffer(batch_positions_x.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
        ispc::parition_as_gather(batch_positions_x.cast<int>().data(), partition.data(), buffer.data(), pertition_mapping_total);
        
        // ispc::gather_ints_buffer_segmented(batch_positions_x.cast<int>().data(),
        //                                    partition.data(),
        //                                    buffer.data(),
        //                                    prefix_to_visit,
        //                                    &front_indices,
        //                                    &back_indices);
        // ispc::gather_ints_buffer(batch_positions_y.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
        // ispc::scatter_ints_buffer(batch_positions_y.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
        ispc::parition_as_gather(batch_positions_y.cast<int>().data(), partition.data(), buffer.data(), pertition_mapping_total);
        
        // ispc::gather_ints_buffer_segmented(batch_positions_y.cast<int>().data(),
        //                                    partition.data(),
        //                                    buffer.data(),
        //                                    prefix_to_visit,
        //                                    &front_indices,
        //                                    &back_indices);
        
        // /* Sice #partition indices are not needed after this gather its possible to use them as buffer instead of other extra memory. */
        // ispc::gather_ints_buffer(batch_positions_z.cast<int>().data(), partition.data(), partition.data(), prefix_to_visit, total_next);
        // ispc::gather_ints_buffer(batch_positions_z.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
        // ispc::scatter_ints_buffer(batch_positions_z.cast<int>().data(), partition.data(), buffer.data(), prefix_to_visit, total_next);
        ispc::parition_as_gather(batch_positions_z.cast<int>().data(), partition.data(), buffer.data(), pertition_mapping_total);
        
        // ispc::gather_ints_buffer_segmented(batch_positions_z.cast<int>().data(),
        //                                    partition.data(),
        //                                    buffer.data(),
        //                                    prefix_to_visit,
        //                                    &front_indices,
        //                                    &back_indices);
      }

      if (UNLIKELY(total_next == 0)) {
        continue;
      }

      if (UNLIKELY(depth_i == total_depth - 1)) {
        continue;
      }

      depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
      joint_stack.extend_unchecked({joint_i * 2 + 1, joint_i * 2 + 0});
      prefix_to_visit_stack.extend_unchecked({total_next, total_next});
    }

    if (value_type.is<float>()) {
      array_utils::scatter<float, int>(batch_values_buffer[0].as_span(), batch_indices_buffer.as_span(), dst_buckets_data.typed<float>());
    } else {
      for (const int i : IndexRange(batch_size)) {
        const int index = batch_indices_buffer[i];
        dst_buckets_data.typed<float3>()[index].x = batch_values_buffer[0][i];
        dst_buckets_data.typed<float3>()[index].y = batch_values_buffer[1][i];
        dst_buckets_data.typed<float3>()[index].z = batch_values_buffer[2][i];
      }
    }
  }

  // Vector<float, 0, GuardedAlignedAllocator<>> buffer;
  // 
  // to_static_type(src_joints_value.type(), [&](auto dummy) {
  //   // SCOPED_TIMER_AVERAGED("  bucket_to_batch_samples");
  //   using T = decltype(dummy);
  // 
  //   const Span<T> typed_src_joints_value = src_joints_value.typed<T>();
  //   const Span<T> typed_src_bucket_value = src_bucket_value.typed<T>();
  //   MutableSpan<T> typed_dst_buckets_data = dst_buckets_data.typed<T>();
  // 
  //   buffer.resize(std::accumulate(bucket_to_batch_samples.begin(), bucket_to_batch_samples.end(), 0, [&](const int size, const auto &item) {
  //     return size + item.first.size() * item.second.size();
  //   }));
  // 
  //   int offset_iter = 0;
  //   for (const auto &[bucket_range, batch_samples] : bucket_to_batch_samples) {
  // 
  //     for (const int sample_index : batch_samples) {
  //       MutableSpan<float> buffer_section = buffer.as_mutable_span().slice(offset_iter, bucket_range.size());
  //       offset_iter += bucket_range.size();
  // 
  //       const float3 position = sample_position[sample_index];
  // 
  //       ispc::distances(const_cast<float (*)[3] >(src_bucket_position.slice(bucket_range).cast<float [3]>().data()),
  //                       position,
  //                       buffer_section.size(),
  //                       buffer_section.data(),
  //                       offset_value);
  //     }
  //   }
  // 
  //   offset_iter = 0;
  //   for (const auto &[bucket_range, batch_samples] : bucket_to_batch_samples) {
  // 
  //     for (const int sample_index : batch_samples) {
  //       MutableSpan<float> buffer_section = buffer.as_mutable_span().slice(offset_iter, bucket_range.size());
  //       offset_iter += bucket_range.size();
  // 
  //       if (bucket_range.contains(sampler_to_bucket_range.value()[sample_index])) {
  //         const int sampler_in_bucket_index = sampler_to_bucket_range.value()[sample_index] -
  //                                             bucket_range.start();
  //         buffer_section[sampler_in_bucket_index] = 0.0f;
  //       }
  //     }
  //   }
  // 
  //   distance_invertion(power_value, buffer.as_mutable_span());
  // 
  //   offset_iter = 0;
  //   for (const auto &[bucket_range, batch_samples] : bucket_to_batch_samples) {
  //     for (const int sample_index : batch_samples) {
  //       const Span<float> buffer_section = buffer.as_span().slice(offset_iter, bucket_range.size());
  //       offset_iter += bucket_range.size();
  //       typed_dst_buckets_data[sample_index] += dot_product<T>(typed_src_bucket_value.slice(bucket_range), buffer_section);
  //     }
  //   }
  // });
}

}  // namespace blender::geometry::fmm
