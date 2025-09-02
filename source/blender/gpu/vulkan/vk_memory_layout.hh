/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#pragma once

#include "gpu_shader_create_info.hh"

#include "BLI_math_base.h"
#include "BLI_math_vector_types.hh"

namespace blender::gpu {

/**
 * Information about alignment/components and memory size for types when using std140 layout.
 */
struct Std140 {
  /** Get the memory size in bytes of a single component using by the given type. */
  static uint32_t component_mem_size(const shader::Type type);
  /** Get to alignment of the given type in bytes. */
  static uint32_t element_alignment(const shader::Type type, bool is_array);
  /** Get the number of components that should be allocated for the given type. */
  static uint32_t element_components_len(const shader::Type type);
  /** Get the number of components of the given type when used in an array. */
  static uint32_t array_components_len(const shader::Type type);
  /**
   * After how many input values do we expect row padding.
   *
   * Mat3 in Std430 needs to be padded so every row would contain 4 floats.
   * inner_row_padding would contain `3` in this case, otherwise this is set to 0.
   */
  static uint32_t inner_row_padding(const shader::Type type);
};

/**
 * Information about alignment/components and memory size for types when using std430 layout.
 */
struct Std430 {
  /** Get the memory size in bytes of a single component using by the given type. */
  static uint32_t component_mem_size(const shader::Type type);
  /** Get to alignment of the given type in bytes. */
  static uint32_t element_alignment(const shader::Type type, bool is_array);
  /** Get the number of components that should be allocated for the given type. */
  static uint32_t element_components_len(const shader::Type type);
  /** Get the number of components of the given type when used in an array. */
  static uint32_t array_components_len(const shader::Type type);
  /**
   * After how many input values do we expect row padding.
   *
   * Mat3 in Std430 needs to be padded so every row would contain 4 floats.
   * inner_row_padding would contain `3` in this case, otherwise this is set to 0.
   */
  static uint32_t inner_row_padding(const shader::Type type);
};

template<typename LayoutT> static uint32_t element_stride(const shader::Type type)
{
  return LayoutT::element_components_len(type) * LayoutT::component_mem_size(type);
}

template<typename LayoutT> static uint32_t array_stride(const shader::Type type)
{
  return LayoutT::array_components_len(type) * LayoutT::component_mem_size(type);
}

/**
 * Move the r_offset to the next alignment where the given type+array_size can be
 * reserved.
 *
 * 'type': The type that needs to be aligned.
 * 'array_size': The array_size that needs to be aligned. (0=no array).
 * 'r_offset': After the call it will point to the byte where the reservation
 *     can happen.
 */
template<typename LayoutT>
static void align(const shader::Type &type, const int32_t array_size, uint32_t *r_offset)
{
  uint32_t alignment = LayoutT::element_alignment(type, array_size != 0);
  uint32_t alignment_mask = alignment - 1;
  uint32_t offset = *r_offset;
  if ((offset & alignment_mask) != 0) {
    offset &= ~alignment_mask;
    offset += alignment;
    *r_offset = offset;
  }
}

/**
 * Reserve space for the given type and array size.
 *
 * This function doesn't handle alignment this needs to be done up front by calling
 * 'align<Layout>' function. Caller is responsible for this.
 *
 * 'type': The type that needs to be reserved.
 * 'array_size': The array_size that needs to be reserved. (0=no array).
 * 'r_offset': When calling needs to be pointing to the aligned location where to
 *     reserve space. After the call it will point to the byte just after reserved
 *     space.
 */
template<typename LayoutT>
static void reserve(const shader::Type type, int32_t array_size, uint32_t *r_offset)
{
  uint32_t size = array_size == 0 ? element_stride<LayoutT>(type) :
                                    array_stride<LayoutT>(type) * array_size;
  *r_offset += size;
}

/**
 * Update 'r_offset' to be aligned to the end of the struct.
 *
 * Call this function when all attributes have been added to make sure that the struct size is
 * correct.
 */
template<typename LayoutT> static void align_end_of_struct(uint32_t *r_offset)
{
  align<LayoutT>(shader::Type::float4_t, 0, r_offset);
}

/**
 * Image transfers can require downloads to happen in smaller chunks.
 *
 * This helper class helps in splitting the transfers in smaller chunks.
 */
struct TransferRegion {
  int3 offset;
  int3 extent;
  IndexRange layers;

  int64_t sample_count() const
  {
    return int64_t(extent.x) * int64_t(extent.y) * int64_t(extent.z) * layers.size();
  }

  /** Split the current region. first on layers, then z extent then y extent. */
  TransferRegion split()
  {
    int3 offset_a;
    int3 offset_b;
    int3 extent_a;
    int3 extent_b;
    IndexRange layers_a;
    IndexRange layers_b;

    if (layers.size() > 1) {
      int split_after = layers.first() + divide_floor_i(layers.last() - layers.first(), 2);
      offset_a = offset;
      offset_b = offset;
      extent_a = extent;
      extent_b = extent;
      layers_a = IndexRange::from_begin_end(layers.first(), split_after);
      layers_b = IndexRange::from_begin_end_inclusive(split_after, layers.last());
    }
    else if (extent.z > 1) {
      int split_after = divide_floor_i(extent.z, 2);
      offset_a = offset;
      offset_b = {offset.x, offset.y, offset.z + split_after};
      extent_a = {extent.x, extent.y, split_after};
      extent_b = {extent.x, extent.y, extent.z - split_after};
      layers_a = layers;
      layers_b = layers;
    }
    else if (extent.y > 1) {
      int split_after = divide_floor_i(extent.y, 2);
      offset_a = offset;
      offset_b = {offset.x, offset.y + split_after, offset.z};
      extent_a = {extent.x, split_after, extent.z};
      extent_b = {extent.x, extent.y - split_after, extent.z};
      layers_a = layers;
      layers_b = layers;
    }
    else {
      BLI_assert_unreachable();
    }

    offset = offset_a;
    extent = extent_a;
    layers = layers_a;
    return {offset_b, extent_b, layers_b};
  }

  bool is_sequential(const TransferRegion &other) const
  {
    BLI_assert_unreachable();
    return true;
  }

  size_t result_offset(int3 offset, int layer) const
  {
    BLI_assert_unreachable();
    return 0;
  }
};

}  // namespace blender::gpu
