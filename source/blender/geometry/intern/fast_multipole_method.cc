/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array_utils.hh"
#include "BLI_generic_span.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_function_ref.hh"
#include "BLI_offset_indices.hh"
#include "BLI_task.hh"

#include "GEO_abstract_kd_bucket_hierarchy.hh"
#include "GEO_bounding_sphere.hh"
#include "GEO_fast_multipole_method.hh"

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
        const float power_factor = float(power_value);
        std::transform(values.begin(), values.end(), values.begin(), [power_factor](const float value) {
          return std::expf(std::logf(value) * power_factor);
        });
      };
  }
}

template<typename LeafFuncT, typename JointPredicateT, typename JointFuncT>
static void for_each_to_bottom_skip(const OffsetIndices<int> buckets_offsets,
                                    const int total_depth,
                                    const IndexRange range,
                                    const JointPredicateT &joint_predicate,
                                    const JointFuncT &joint_func,
                                    const LeafFuncT &leaf_func)
{
  Array<int, 0> indices(range.size());
  array_utils::fill_index_range<int>(indices, range.start());

  Vector<int, 32> depth_stack({0});
  Vector<int, 32> joint_stack({0});
  Vector<int, 32> prefix_to_visit_stack({int(indices.size())});

  while (!depth_stack.is_empty()) {
    const int prefix_to_visit = prefix_to_visit_stack.pop_last();
    const int depth_i = depth_stack.pop_last();
    const int joint_i = joint_stack.pop_last();
    const MutableSpan<int> to_visit = indices.as_mutable_span().take_front(prefix_to_visit);
    const IndexRange joints_range = akdbh::joints_range_at_depth(depth_i);

    const auto end_of_prefix = std::stable_partition(to_visit.begin(), to_visit.end(), [&](const int i) -> bool {
      return joint_predicate(int(joints_range[joint_i]), i);
    });

    const int num_to_visit_next = std::distance(to_visit.begin(), end_of_prefix);
    const Span<int> finished_indices = to_visit.drop_front(num_to_visit_next);
    joint_func(int(joints_range[joint_i]), finished_indices);

    const Span<int> next_indices = to_visit.take_front(num_to_visit_next);
    if (next_indices.is_empty()) {
      continue;
    }

    if (depth_i == total_depth - 1) {
      leaf_func(buckets_offsets[joint_i], next_indices);
      continue;
    }

    depth_stack.extend_unchecked({depth_i + 1, depth_i + 1});
    joint_stack.extend_unchecked({joint_i * 2 + 1, joint_i * 2 + 0});
    prefix_to_visit_stack.extend_unchecked({num_to_visit_next, num_to_visit_next});
  }
}

void akdbh_sample_value(OffsetIndices<int> buckets_offsets,
                        const int total_depth,
                        const Span<float3> src_joints_centre,
                        const Span<float3> src_bucket_position,
                        const Span<float> src_joints_min_distance_reduced,
                        const GSpan src_joints_value,
                        const GSpan src_bucket_value,
                        const int power_value,
                        const float offset_value,
                        GMutableSpan dst_buckets_data)
{
  BLI_assert(src_joints_centre.size() == src_joints_min_distance_reduced.size());
  BLI_assert(src_joints_centre.size() == src_joints_value.size());
  BLI_assert(src_bucket_value.size() == dst_buckets_data.size());
  BLI_assert(src_bucket_value.size() == src_bucket_position.size());
  BLI_assert(dst_buckets_data.type() == src_joints_value.type());
  BLI_assert(dst_buckets_data.type() == src_bucket_value.type());

  const FunctionRef<void(int, MutableSpan<float>)> distance_invertion = powered_rcp_for_values(
      power_value);

  threading::parallel_for(
      src_bucket_position.index_range(), 1024 * 8, [&](const IndexRange range) {
        Vector<std::pair<int, Vector<int, 0>>, 0> joint_to_buckets;
        Vector<std::pair<IndexRange, Vector<int>>, 0> bucket_to_joints;

        for_each_to_bottom_skip(
            buckets_offsets,
            total_depth,
            range,
            [&](const int joint_index, const int value_i) -> bool {
              return math::distance(src_joints_centre[joint_index],
                                    src_bucket_position[value_i]) <=
                     src_joints_min_distance_reduced[joint_index];
            },
            [&](const int joint_index,
                const Span<int> value_indices) {
              joint_to_buckets.append_as(joint_index, value_indices);
            },
            [&](const IndexRange bucket_range, const Span<int> value_indices) {
              bucket_to_joints.append_as(bucket_range, value_indices);
            });

        Vector<float, 0> buffer;
        buffer.reserve(range.size());

        to_static_type(src_joints_value.type(), [&](auto dummy) {
          using T = decltype(dummy);

          const Span<T> typed_src_joints_value = src_joints_value.typed<T>();
          const Span<T> typed_src_bucket_value = src_bucket_value.typed<T>();
          MutableSpan<T> typed_dst_buckets_data = dst_buckets_data.typed<T>();

          for (const auto &[joint_index, value_indices] : joint_to_buckets) {
            buffer.resize(value_indices.size());
            for (const int value_i : value_indices.index_range()) {
              const int value_index = value_indices[value_i];
              buffer[value_i] = math::distance(src_joints_centre[joint_index], src_bucket_position[value_index]) + offset_value;
            }

            distance_invertion(power_value, buffer.as_mutable_span());

            for (const int value_i : value_indices.index_range()) {
              const int value_index = value_indices[value_i];
              typed_dst_buckets_data[value_index] += typed_src_joints_value[joint_index] * buffer[value_i];
            }
          }

          for (const auto &[bucket_range, value_indices] : bucket_to_joints) {
            buffer.resize(bucket_range.size());
            for (const int value_i : value_indices) {
              const float3 position = src_bucket_position[value_i];

              for (const int index : bucket_range.index_range()) {
                buffer[index] = math::distance(src_bucket_position[bucket_range[index]], position) + offset_value;
              }

              distance_invertion(power_value, buffer.as_mutable_span());

              for (const int i : bucket_range.index_range()) {
                const int index = bucket_range[i];
                const float relation_factor = buffer[i];
                const float safe_relation_factor = index == value_i ? 0.0f : relation_factor;
                typed_dst_buckets_data[value_i] += typed_src_bucket_value[index] * safe_relation_factor;
              }
            }
          }
        });
      });
}

/*
void akdbh_sample_value(OffsetIndices<int> buckets_offsets,
                        const int total_depth,
                        const Span<float3> src_joints_centre,
                        const Span<float3> src_bucket_position,
                        const Span<float> src_joints_min_distance_reduced,
                        const GSpan src_joints_value,
                        const GSpan src_bucket_value,
                        const int power_value,
                        const float offset_value,
                        GMutableSpan dst_buckets_data)
{
  BLI_assert(src_joints_centre.size() == src_joints_min_distance_reduced.size());
  BLI_assert(src_joints_centre.size() == src_joints_value.size());
  BLI_assert(src_bucket_value.size() == dst_buckets_data.size());
  BLI_assert(src_bucket_value.size() == src_bucket_position.size());
  BLI_assert(dst_buckets_data.type() == src_joints_value.type());
  BLI_assert(dst_buckets_data.type() == src_bucket_value.type());

  const FunctionRef<void(int, MutableSpan<float>)> distance_invertion = powered_rcp_for_values(
      power_value);

  threading::parallel_for(
      src_bucket_position.index_range(), 1024 * 8, [&](const IndexRange range) {
        Vector<std::pair<int, Vector<int, 0>>, 0> joint_to_buckets;
        Vector<std::pair<IndexRange, Vector<int>>, 0> bucket_to_joints;

        for_each_to_bottom_skip(
            buckets_offsets,
            total_depth,
            range,
            [&](const int joint_index, const int value_i) -> bool {
              return math::distance(src_joints_centre[joint_index],
                                    src_bucket_position[value_i]) <=
                     src_joints_min_distance_reduced[joint_index];
            },
            [&](const int joint_index,
                const Span<int> value_indices) {
              joint_to_buckets.append_as(joint_index, value_indices);
            },
            [&](const IndexRange bucket_range, const Span<int> value_indices) {
              bucket_to_joints.append_as(bucket_range, value_indices);
            });

        Vector<float, 0> buffer;
        buffer.reserve(range.size());

        to_static_type(src_joints_value.type(), [&](auto dummy) {
          using T = decltype(dummy);

          const Span<T> typed_src_joints_value = src_joints_value.typed<T>();
          const Span<T> typed_src_bucket_value = src_bucket_value.typed<T>();
          MutableSpan<T> typed_dst_buckets_data = dst_buckets_data.typed<T>();

          for (const auto &[joint_index, value_indices] : joint_to_buckets) {
            buffer.resize(value_indices.size());
            for (const int value_i : value_indices.index_range()) {
              const int value_index = value_indices[value_i];
              buffer[value_i] = math::distance(src_joints_centre[joint_index], src_bucket_position[value_index]) + offset_value;
            }

            distance_invertion(power_value, buffer.as_mutable_span());

            for (const int value_i : value_indices.index_range()) {
              const int value_index = value_indices[value_i];
              typed_dst_buckets_data[value_index] += typed_src_joints_value[joint_index] * buffer[value_i];
            }
          }

          for (const auto &[bucket_range, value_indices] : bucket_to_joints) {
            buffer.resize(bucket_range.size());
            for (const int value_i : value_indices) {
              const float3 position = src_bucket_position[value_i];

              for (const int index : bucket_range.index_range()) {
                buffer[index] = math::distance(src_bucket_position[bucket_range[index]], position) + offset_value;
              }

              distance_invertion(power_value, buffer.as_mutable_span());

              for (const int i : bucket_range.index_range()) {
                const int index = bucket_range[i];
                const float relation_factor = buffer[i];
                const float safe_relation_factor = index == value_i ? 0.0f : relation_factor;
                typed_dst_buckets_data[value_i] += typed_src_bucket_value[index] * safe_relation_factor;
                typed_dst_buckets_data[value_i] += typed_src_bucket_value[index] * buffer[i];
              }
            }
          }
        });
      });
}
*/

}  // namespace blender::geometry::fmm
