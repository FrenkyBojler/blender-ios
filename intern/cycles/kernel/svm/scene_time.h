/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/svm/util.h"

CCL_NAMESPACE_BEGIN

ccl_device_noinline void svm_node_scene_time(KernelGlobals kg,
                                             ccl_private float *stack,
                                             const uint seconds_out,
                                             const uint frame_out)
{
  if (stack_valid(seconds_out)) {
    stack_store_float(stack, seconds_out, kernel_data.scene_time.time);
  }
  if (stack_valid(frame_out)) {
    stack_store_float(stack, frame_out, kernel_data.scene_time.frame);
  }
}

CCL_NAMESPACE_END
