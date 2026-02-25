/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/svm/util.h"

#include "kernel/util/colorspace.h"

CCL_NAMESPACE_BEGIN

/* Conversion Nodes */

template<bool derivative>
ccl_device_noinline void svm_node_convert(
    KernelGlobals kg, ccl_private float *stack, const uint type, const uint from, const uint to)
{
  /* Conditional types: use plain types when derivative=false, dual when derivative=true. */
  using F = ccl_conditional_t<derivative, dual1, float>;
  using V = ccl_conditional_t<derivative, dual3, float3>;

  switch ((NodeConvert)type) {
    case NODE_CONVERT_FI: {
      /* TODO(weizhen): should actually store 0 for int, but none of the nodes that we compute
       * derivatives for has int inputs, so seems fine. */
      const float f = stack_load_float(stack, from);
      stack_store_int(stack, to, float_to_int(f));
      break;
    }
    case NODE_CONVERT_FV: {
      const F f = stack_load<F>(stack, from);
      stack_store(stack, to, make_float3(f, f, f));
      break;
    }
    case NODE_CONVERT_CF: {
      if constexpr (derivative) {
        const dual3 f = stack_load<dual3>(stack, from);
        const dual1 g = linear_rgb_to_gray(kg, f);
        stack_store(stack, to, g);
      }
      else {
        const float3 f = stack_load_float3(stack, from);
        const float g = linear_rgb_to_gray(kg, f);
        stack_store_float(stack, to, g);
      }
      break;
    }
    case NODE_CONVERT_CI: {
      const float3 f = stack_load_float3(stack, from);
      const int i = (int)linear_rgb_to_gray(kg, f);
      stack_store_int(stack, to, i);
      break;
    }
    case NODE_CONVERT_VF: {
      if constexpr (derivative) {
        const dual3 f = stack_load<dual3>(stack, from);
        const dual1 g = average(f);
        stack_store(stack, to, g);
      }
      else {
        const float3 f = stack_load_float3(stack, from);
        const float g = average(f);
        stack_store_float(stack, to, g);
      }
      break;
    }
    case NODE_CONVERT_VI: {
      const float3 f = stack_load_float3(stack, from);
      const int i = (int)average(f);
      stack_store_int(stack, to, i);
      break;
    }
    case NODE_CONVERT_IF: {
      const float f = (float)stack_load_int(stack, from);
      stack_store(stack, to, F(f));
      break;
    }
    case NODE_CONVERT_IV: {
      const float f = (float)stack_load_int(stack, from);
      stack_store(stack, to, V(make_float3(f, f, f)));
      break;
    }
    default:
      assert(false);
  }
}

CCL_NAMESPACE_END
