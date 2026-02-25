/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/svm/util.h"

CCL_NAMESPACE_BEGIN

/* Vector combine / separate, used for the RGB and XYZ nodes */

template<typename T>
ccl_device void svm_node_combine_vector(ccl_private float *stack,
                                        const uint in_offset,
                                        const uint vector_index,
                                        const uint out_offset)
{
  using S = scalar_t<T>;
  const S value = stack_load<S>(stack, in_offset);

  if (stack_valid(out_offset)) {
    if constexpr (is_dual_v(T)) {
      stack_store_float(stack, out_offset + vector_index, value.val);
      stack_store_float(stack, out_offset + vector_index + 3, value.dx);
      stack_store_float(stack, out_offset + vector_index + 6, value.dy);
    }
    else {
      stack_store_float(stack, out_offset + vector_index, value);
    }
  }
}

template<typename T>
ccl_device void svm_node_separate_vector(ccl_private float *stack,
                                         const uint ivector_offset,
                                         const uint vector_index,
                                         const uint out_offset)
{
  const T vector = stack_load<T>(stack, ivector_offset);

  if (stack_valid(out_offset)) {
    if constexpr (is_dual_v(T)) {
      if (vector_index == 0) {
        stack_store(stack, out_offset, vector.x());
      }
      else if (vector_index == 1) {
        stack_store(stack, out_offset, vector.y());
      }
      else {
        stack_store(stack, out_offset, vector.z());
      }
    }
    else {
      if (vector_index == 0) {
        stack_store(stack, out_offset, vector.x);
      }
      else if (vector_index == 1) {
        stack_store(stack, out_offset, vector.y);
      }
      else {
        stack_store(stack, out_offset, vector.z);
      }
    }
  }
}

CCL_NAMESPACE_END
