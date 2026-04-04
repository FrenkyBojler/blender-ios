/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/svm/util.h"

CCL_NAMESPACE_BEGIN

ccl_device_noinline void svm_node_scene_time(ccl_private float *stack,
                                             const uint seconds_out,
                                             const uint frame_out)
{
  stack_store_float(stack, seconds_out, 0.5);
  stack_store_float(stack, frame_out, 1.0); 
}

CCL_NAMESPACE_END
