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

namespace blender::ispc_math {

constexpr int programCount = ispc::FMMConstants::ChunkSize;

static void distance_to_n(const Span<float> src_a_x,
                          const Span<float> src_a_y,
                          const Span<float> src_a_z,
                          const float3 src_b_xyz,
                          MutableSpan<float> dst)
{
  BLI_assert(src_a_x.size() == src_a_y.size());
  BLI_assert(src_a_x.size() == src_a_z.size());
  BLI_assert(src_a_x.size() == dst.size());

  ispc::distance_to_n(
      src_a_x.data(), src_a_y.data(), src_a_z.data(), src_b_xyz, src_a_z.size(), dst.data());
}

static void chunked_squared_distances_table(const Span<float[programCount]> row_x,
                                            const Span<float[programCount]> row_y,
                                            const Span<float[programCount]> row_z,
                                            const Span<float> col_x,
                                            const Span<float> col_y,
                                            const Span<float> col_z,
                                            MutableSpan<float[programCount]> distances)
{
  BLI_assert(row_x.size() == row_y.size());
  BLI_assert(row_x.size() == row_z.size());

  BLI_assert(col_x.size() == col_y.size());
  BLI_assert(col_x.size() == col_z.size());

  BLI_assert(distances.size() == row_x.size() * col_z.size());

  ispc::chunked_squared_distances_table(row_x.data(),
                                        row_y.data(),
                                        row_z.data(),
                                        row_z.size(),
                                        col_x.data(),
                                        col_y.data(),
                                        col_z.data(),
                                        col_z.size(),
                                        distances.data());
}

static void squared_distances_table(const Span<float> row_x,
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

  ispc::squared_distances_table(row_x.data(),
                                row_y.data(),
                                row_z.data(),
                                row_z.size(),
                                col_x.data(),
                                col_y.data(),
                                col_z.data(),
                                col_z.size(),
                                distances.data());
}

static void chunked_zero_if_index_in_range(const Span<int[programCount]> row_indices,
                                           const ShiftedRange col_range,
                                           MutableSpan<float[programCount]> rows_and_cols)
{
  BLI_assert(col_range.size * row_indices.size() == rows_and_cols.size());

  ispc::chunked_zero_if_index_in_range(row_indices.data(),
                                       col_range.size,
                                       col_range.start,
                                       row_indices.size(),
                                       rows_and_cols.data());
}

static void zero_if_index_in_range(const Span<int> row_indices,
                                   const ShiftedRange col_range,
                                   MutableSpan<float> rows_and_cols)
{
  BLI_assert(col_range.size * row_indices.size() == rows_and_cols.size());

  ispc::zero_if_index_in_range(row_indices.data(),
                               col_range.size,
                               col_range.start,
                               row_indices.size(),
                               rows_and_cols.data());
}

static void chunked_table_product_reduce(const Span<float[programCount]> rows_and_cols,
                                         const Span<float> col_values,
                                         MutableSpan<float[programCount]> row_values)
{
  BLI_assert(rows_and_cols.size() == col_values.size() * row_values.size());

  ispc::chunked_table_product_reduce(rows_and_cols.data(),
                                     col_values.size(),
                                     col_values.data(),
                                     row_values.size(),
                                     row_values.data());
}

static void table_product_reduce(const Span<float> rows_and_cols,
                                 const Span<float> col_values,
                                 MutableSpan<float> row_values)
{
  BLI_assert(rows_and_cols.size() == col_values.size() * row_values.size());

  ispc::table_product_reduce(rows_and_cols.data(),
                             col_values.size(),
                             col_values.data(),
                             row_values.size(),
                             row_values.data());
}

template<typename T>
static void scatter(const Span<T> src, const Span<int> indices, MutableSpan<T> dst)
{
  BLI_assert(src.size() == indices.size());
  for (const int i : src.index_range()) {
    dst[indices[i]] = src[i];
  }
}

template<typename T>
static void gather(const Span<T> src, const Span<int> indices, MutableSpan<T> dst)
{
  BLI_assert(dst.size() == indices.size());
  for (const int i : dst.index_range()) {
    dst[i] = src[indices[i]];
  }
}

template<typename T>
static void parition_as_gather(MutableSpan<T> values,
                               const Span<int> front_indices,
                               const Span<int> back_indices,
                               MutableSpan<T> buffer)
{
  BLI_assert(front_indices.size() == back_indices.size());
  BLI_assert(front_indices.size() + back_indices.size() == buffer.size());
  BLI_assert(values.size() >= buffer.size());

  gather<T>(values, front_indices, buffer.take_front(front_indices.size()));
  gather<T>(values, back_indices, buffer.drop_front(front_indices.size()));

  scatter<T>(buffer.take_front(front_indices.size()), back_indices, values);
  scatter<T>(buffer.drop_front(front_indices.size()), front_indices, values);
}

template<typename T>
static void parition_as_gather_front_only(MutableSpan<T> values,
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

static int count_floats_less_than(const Span<float> values, float min_predicate_value)
{
  return ispc::count_floats_less_than(values.data(), min_predicate_value, values.size());
}

static std::pair<Span<int>, Span<int>> predicate_partition_indices_float_cmp(
    MutableSpan<int> indices,
    const Span<float> predicates,
    const float min_predicate_value,
    const int total_front_size)
{
  BLI_assert(total_front_size > 0);
  BLI_assert(total_front_size <= predicates.size());
  BLI_assert(indices.size() <= predicates.size());

  const int total = ispc::predicate_partition_indices_float_cmp(
      indices.data(), predicates.data(), predicates.size(), min_predicate_value, total_front_size);

  return {indices.slice(0, total / 2), indices.slice(total / 2, total / 2)};
}

static void mul_n_add_to(MutableSpan<float> dst, const Span<float> src, const float value)
{
  BLI_assert(dst.size() == src.size());
  ispc::mul_n_add_to(dst.data(), src.data(), value, src.size());
}

}  // namespace blender::ispc_math

namespace blender::math {

constexpr int programCount = 16;

static void distance_to_n(const Span<float> src_a_x,
                          const Span<float> src_a_y,
                          const Span<float> src_a_z,
                          const float3 src_b_xyz,
                          MutableSpan<float> dst)
{
  BLI_assert(src_a_x.size() == src_a_y.size());
  BLI_assert(src_a_x.size() == src_a_z.size());
  BLI_assert(src_a_x.size() == dst.size());

  for (const int i : src_a_x.index_range()) {
    dst[i] = math::distance(float3(src_a_x[i], src_a_y[i], src_a_z[i]), src_b_xyz);
  }
}

static void chunked_squared_distances_table(const Span<float[programCount]> row_x,
                                            const Span<float[programCount]> row_y,
                                            const Span<float[programCount]> row_z,
                                            const Span<float> col_x,
                                            const Span<float> col_y,
                                            const Span<float> col_z,
                                            MutableSpan<float[programCount]> distances)
{
  BLI_assert(row_x.size() == row_y.size());
  BLI_assert(row_x.size() == row_z.size());

  BLI_assert(col_x.size() == col_y.size());
  BLI_assert(col_x.size() == col_z.size());

  BLI_assert(distances.size() == row_x.size() * col_z.size());

  for (const int row_index : row_y.index_range()) {
    for (const int col_index : col_x.index_range()) {
      const float3 col_xyz(col_x[col_index], col_y[col_index], col_z[col_index]);
      for (int programIndex = 0; programIndex < programCount; programIndex++) {
        const float3 row_xyz(row_x[row_index][programIndex],
                             row_y[row_index][programIndex],
                             row_z[row_index][programIndex]);
        const float3 batch = blender::math::square(row_xyz - col_xyz);
        distances[row_index * col_x.size() + col_index][programIndex] = batch.x + batch.y +
                                                                        batch.z;
      }
    }
  }
}

static void squared_distances_table(const Span<float> row_x,
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

  for (const int row_index : row_y.index_range()) {
    const float3 row_xyz(row_x[row_index], row_y[row_index], row_z[row_index]);
    for (const int col_index : col_x.index_range()) {
      const float3 col_xyz(col_x[col_index], col_y[col_index], col_z[col_index]);
      const float3 batch = blender::math::square(row_xyz - col_xyz);
      distances[row_index * col_x.size() + col_index] = batch.x + batch.y + batch.z;
    }
  }
}

static void chunked_zero_if_index_in_range(const Span<int[programCount]> row_indices,
                                           const ShiftedRange col_range,
                                           MutableSpan<float[programCount]> rows_and_cols)
{
  BLI_assert(col_range.size * row_indices.size() == rows_and_cols.size());

  for (const int row_index : row_indices.index_range()) {
    for (const int col_index : col_range.index_range()) {
      for (int programIndex = 0; programIndex < programCount; programIndex++) {
        const int bucket_indices = row_indices[row_index][programIndex];
        const bool mask = bucket_indices != col_range[col_index];
        if (mask) {
          rows_and_cols[row_index * col_range.size + col_index][programIndex] =
              rows_and_cols[row_index * col_range.size + col_index][programIndex];
        }
        else {
          rows_and_cols[row_index * col_range.size + col_index][programIndex] = 0.0f;
        }
      }
    }
  }
}

static void zero_if_index_in_range(const Span<int> row_indices,
                                   const ShiftedRange col_range,
                                   MutableSpan<float> rows_and_cols)
{
  BLI_assert(col_range.size * row_indices.size() == rows_and_cols.size());

  for (const int row_index : row_indices.index_range()) {
    for (const int col_index : col_range.index_range()) {
      const int bucket_indices = row_indices[row_index];
      const bool mask = bucket_indices != col_range[col_index];
      if (mask) {
        rows_and_cols[row_index * col_range.size + col_index] =
            rows_and_cols[row_index * col_range.size + col_index];
      }
      else {
        rows_and_cols[row_index * col_range.size + col_index] = 0.0f;
      }
    }
  }
}

static void chunked_table_product_reduce(const Span<float[programCount]> rows_and_cols,
                                         const Span<float> col_values,
                                         MutableSpan<float[programCount]> row_values)
{
  BLI_assert(rows_and_cols.size() == col_values.size() * row_values.size());
  for (const int row_index : row_values.index_range()) {
    for (const int col_index : col_values.index_range()) {
      for (int programIndex = 0; programIndex < programCount; programIndex++) {
        row_values[row_index][programIndex] +=
            rows_and_cols[row_index * col_values.size() + col_index][programIndex] *
            col_values[col_index];
      }
    }
  }
}

static void table_product_reduce(const Span<float> rows_and_cols,
                                 const Span<float> col_values,
                                 MutableSpan<float> row_values)
{
  BLI_assert(rows_and_cols.size() == col_values.size() * row_values.size());
  for (const int row_index : row_values.index_range()) {
    for (const int col_index : col_values.index_range()) {
      row_values[row_index] += rows_and_cols[row_index * col_values.size() + col_index] *
                               col_values[col_index];
    }
  }
}

template<typename T>
static void scatter(const Span<T> src, const Span<int> indices, MutableSpan<T> dst)
{
  BLI_assert(src.size() == indices.size());
  for (const int i : src.index_range()) {
    dst[indices[i]] = src[i];
  }
}

template<typename T>
static void gather(const Span<T> src, const Span<int> indices, MutableSpan<T> dst)
{
  BLI_assert(dst.size() == indices.size());
  for (const int i : dst.index_range()) {
    dst[i] = src[indices[i]];
  }
}

template<typename T>
static void parition_as_gather(MutableSpan<T> values,
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
static void parition_as_gather_front_only(MutableSpan<T> values,
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

static int count_floats_less_than(const Span<float> values, float min_predicate_value)
{
  return std::count_if(
      values.begin(), values.end(), [&](const float item) { return item < min_predicate_value; });
}

static std::pair<Span<int>, Span<int>> predicate_partition_indices_float_cmp(
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

static void mul_n_add_to(MutableSpan<float> dst, const Span<float> src, const float value)
{
  BLI_assert(dst.size() == src.size());
  for (const int i : dst.index_range()) {
    dst[i] += value * src[i];
  }
}

}  // namespace blender::math

#if (1)
namespace fast_math = blender::math;
#else
namespace fast_math = blender::ispc_math;
#endif

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
    BLI_assert(!sampler_to_bucket_range.has_value() ||
               sampler_to_bucket_range->size() == dst_buckets_data[0].size());
    BLI_assert(!sampler_to_bucket_range.has_value() ||
               src_bucket_position[0].index_range().contains(*sampler_to_bucket_range));
  }
  const FunctionRef<void(int, MutableSpan<float>)> distance_invertion = powered_rcp_for_values(
      power_value);

  const int batch_size = sample_position[0].size();
  const int aligned_batch_size = round_up_for(batch_size, sse_min_alignment);
  const int data_axes_num = src_bucket_value.size();

  Array<float, 0, GuardedAlignedAllocator<sse_min_alignment>> sampler_position_data(
      aligned_batch_size * 3);
  std::array<MutableSpan<float>, 3> batch_positions_data;
  for (const int axis_i : IndexRange(3)) {
    batch_positions_data[axis_i] = sampler_position_data.as_mutable_span().slice(
        aligned_batch_size * axis_i, batch_size);
    batch_positions_data[axis_i].copy_from(sample_position[axis_i]);
  }

  Array<float, 0, GuardedAlignedAllocator<sse_min_alignment>> sampler_value_data(
      aligned_batch_size * data_axes_num);
  Array<MutableSpan<float>, 3> batch_values_data(data_axes_num);
  for (const int axis_i : IndexRange(data_axes_num)) {
    batch_values_data[axis_i] = sampler_value_data.as_mutable_span().slice(
        aligned_batch_size * axis_i, batch_size);
    batch_values_data[axis_i].fill(0);
  }

  Array<int, 0, GuardedAlignedAllocator<sse_min_alignment>> sampler_mapping_data(
      aligned_batch_size * 3);

  MutableSpan<int> batch_indices_data = sampler_mapping_data.as_mutable_span().slice(
      aligned_batch_size * 0, batch_size);
  MutableSpan<int> partition_indices_buffer = sampler_mapping_data.as_mutable_span().slice(
      aligned_batch_size * 1, batch_size);
  MutableSpan<int> partition_buffer_data = sampler_mapping_data.as_mutable_span().slice(
      aligned_batch_size * 2, batch_size);

  array_utils::fill_index_range<int>(batch_indices_data, 0);

  Vector<float, 0, GuardedAlignedAllocator<sse_min_alignment>> batch_distances_buffer(batch_size);

  Vector<float, 0, GuardedAlignedAllocator<sse_min_alignment>> bucket_position_data;

  static_assert(sizeof(int) == sizeof(float),
                "Some ISPC functions are reused for int and float data");
  static_assert(alignof(int) == alignof(float),
                "Some ISPC functions are reused for int and float data");

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

    const MutableSpan<float> batch_positions_x = batch_positions_data[0].take_front(
        prefix_to_visit);
    const MutableSpan<float> batch_positions_y = batch_positions_data[1].take_front(
        prefix_to_visit);
    const MutableSpan<float> batch_positions_z = batch_positions_data[2].take_front(
        prefix_to_visit);

    batch_distances_buffer.reinitialize(prefix_to_visit);

    fast_math::distance_to_n(batch_positions_x,
                             batch_positions_y,
                             batch_positions_z,
                             joint_position,
                             batch_distances_buffer);

    const int total_to_pass_to_childs = fast_math::count_floats_less_than(
        batch_distances_buffer, joint_min_distance - offset_value);

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

      std::optional<std::pair<Span<int>, Span<int>>> partition_mapping;
      std::optional<MutableSpan<int>> partition_buffer;
      if (!all_end_on_joint) {
        partition_mapping = fast_math::predicate_partition_indices_float_cmp(
            partition,
            batch_distances_buffer,
            joint_min_distance - offset_value,
            total_to_pass_to_childs);
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

      for (const int i : IndexRange(prefix_to_visit - total_to_pass_to_childs)) {
        batch_distances_buffer[i] += offset_value;
      }

      distance_invertion(power_value,
                         batch_distances_buffer.as_mutable_span().take_front(
                             prefix_to_visit - total_to_pass_to_childs));

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
      bucket_position_data.as_mutable_span()
          .slice(aligned_bucket_size * axis_i, bucket_size)
          .copy_from(src_bucket_position[axis_i].slice(joint_backet));
    }
    const Span<float> bucket_positions_x = bucket_position_data.as_mutable_span().slice(
        aligned_bucket_size * 0, bucket_size);
    const Span<float> bucket_positions_y = bucket_position_data.as_mutable_span().slice(
        aligned_bucket_size * 1, bucket_size);
    const Span<float> bucket_positions_z = bucket_position_data.as_mutable_span().slice(
        aligned_bucket_size * 2, bucket_size);

    constexpr int chunk_size = fast_math::programCount;
    const int chunked_batch_size = round_for(total_to_pass_to_childs, chunk_size);

    const Span<float[chunk_size]> chunked_batch_x =
        batch_positions_x.take_front(chunked_batch_size).cast<float[chunk_size]>();
    const Span<float[chunk_size]> chunked_batch_y =
        batch_positions_y.take_front(chunked_batch_size).cast<float[chunk_size]>();
    const Span<float[chunk_size]> chunked_batch_z =
        batch_positions_z.take_front(chunked_batch_size).cast<float[chunk_size]>();

    const Span<float> rest_batch_x =
        batch_positions_x.take_front(total_to_pass_to_childs).drop_front(chunked_batch_size);
    const Span<float> rest_batch_y =
        batch_positions_y.take_front(total_to_pass_to_childs).drop_front(chunked_batch_size);
    const Span<float> rest_batch_z =
        batch_positions_z.take_front(total_to_pass_to_childs).drop_front(chunked_batch_size);

    BLI_assert(rest_batch_x.size() < chunk_size);

    batch_distances_buffer.reinitialize(
        bucket_size * (chunked_batch_x.size() * chunk_size + rest_batch_x.size()));
    const int total_chunked_table_size = bucket_size * chunked_batch_x.size() * chunk_size;

#ifndef NDEBUG
    batch_distances_buffer.as_mutable_span().fill(-1.0f);
#endif

    const MutableSpan<float[chunk_size]> chunked_distances = batch_distances_buffer
                                                                 .as_mutable_span()
                                                                 .take_front(
                                                                     total_chunked_table_size)
                                                                 .cast<float[chunk_size]>();
    const MutableSpan<float> rest_distances = batch_distances_buffer.as_mutable_span().drop_front(
        total_chunked_table_size);

    BLI_assert(chunked_distances.size() == chunked_batch_x.size() * bucket_size);
    fast_math::chunked_squared_distances_table(chunked_batch_x,
                                               chunked_batch_y,
                                               chunked_batch_z,
                                               bucket_positions_x,
                                               bucket_positions_y,
                                               bucket_positions_z,
                                               chunked_distances);

    BLI_assert(rest_distances.size() == rest_batch_x.size() * bucket_size);
    fast_math::squared_distances_table(rest_batch_x,
                                       rest_batch_y,
                                       rest_batch_z,
                                       bucket_positions_x,
                                       bucket_positions_y,
                                       bucket_positions_z,
                                       rest_distances);
    BLI_assert(!batch_distances_buffer.as_span().contains(-1.0f));

    ispc::sqrt_n_add_single(
        batch_distances_buffer.data(), batch_distances_buffer.size(), offset_value);
    distance_invertion(power_value, batch_distances_buffer.as_mutable_span());

    if (sampler_to_bucket_range.has_value()) {
      const IndexRange range_of_samplers = *sampler_to_bucket_range;
      if (!range_of_samplers.intersect(joint_backet).is_empty()) {
        const Span<int> batch_indices = batch_indices_data.take_front(total_to_pass_to_childs);
        const Span<int[chunk_size]> chunked_batch_indices =
            batch_indices.take_front(chunked_batch_size).cast<int[chunk_size]>();
        const Span<int> rest_batch_indices = batch_indices.drop_front(chunked_batch_size);

        const int from_bucket_to_sampler_offset = joint_backet.start() - range_of_samplers.start();
        fast_math::chunked_zero_if_index_in_range(
            chunked_batch_indices,
            ShiftedRange{from_bucket_to_sampler_offset, bucket_size},
            chunked_distances);
        fast_math::zero_if_index_in_range(rest_batch_indices,
                                          ShiftedRange{from_bucket_to_sampler_offset, bucket_size},
                                          rest_distances);
      }
    }

    for (const int data_i : IndexRange(data_axes_num)) {
      const Span<float> backet_values = src_bucket_value[data_i].slice(joint_backet);

      const MutableSpan<float> batch_values = batch_values_data[data_i].take_front(
          total_to_pass_to_childs);
      const MutableSpan<float[chunk_size]> chunked_batch_values =
          batch_values.take_front(chunked_batch_size).cast<float[chunk_size]>();
      const MutableSpan<float> rest_batch_values = batch_values.drop_front(chunked_batch_size);
      fast_math::chunked_table_product_reduce(
          chunked_distances, backet_values, chunked_batch_values);
      fast_math::table_product_reduce(rest_distances, backet_values, rest_batch_values);
    }
  }

  for (const int data_i : IndexRange(data_axes_num)) {
    array_utils::scatter<float, int>(batch_values_data[data_i].as_span(),
                                     batch_indices_data.as_span(),
                                     dst_buckets_data[data_i]);
  }
}

}  // namespace blender::geometry::fmm
