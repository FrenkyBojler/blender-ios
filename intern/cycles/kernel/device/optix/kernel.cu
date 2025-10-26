/* SPDX-FileCopyrightText: 2019, NVIDIA Corporation
 * SPDX-FileCopyrightText: 2019-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

// clang-format off
#include "kernel/device/optix/compat.h"
#include "kernel/device/optix/globals.h"

#include "kernel/device/gpu/image.h"  /* Texture lookup uses normal CUDA intrinsics. */

#include "kernel/tables.h"

#include "kernel/integrator/state.h"
#include "kernel/integrator/state_flow.h"
#include "kernel/integrator/state_util.h"

#include "kernel/integrator/intersect_closest.h"
#include "kernel/integrator/intersect_shadow.h"
#include "kernel/integrator/intersect_subsurface.h"
#include "kernel/integrator/intersect_volume_stack.h"
#include "kernel/integrator/intersect_dedicated_light.h"

#include "kernel/film/render_time.h"
// clang-format on

#define GPU_TIMER_START() uint64_t _timer_start = gpu_time_fast()
#define GPU_TIMER_END() write_render_time(path_index, kernel_params.render_buffer, _timer_start)

extern "C" __global__ void __raygen__kernel_optix_integrator_intersect_closest()
{
  const int global_index = optixGetLaunchIndex().x;
  const int path_index = (kernel_params.path_index_array) ?
                             kernel_params.path_index_array[global_index] :
                             global_index;
  GPU_TIMER_START();
  integrator_intersect_closest(nullptr, path_index, kernel_params.render_buffer);
  GPU_TIMER_END();
}

extern "C" __global__ void __raygen__kernel_optix_integrator_intersect_shadow()
{
  const int global_index = optixGetLaunchIndex().x;
  const int path_index = (kernel_params.path_index_array) ?
                             kernel_params.path_index_array[global_index] :
                             global_index;
  GPU_TIMER_START();
  integrator_intersect_shadow(nullptr, path_index);
  GPU_TIMER_END();
}

extern "C" __global__ void __raygen__kernel_optix_integrator_intersect_subsurface()
{
  const int global_index = optixGetLaunchIndex().x;
  const int path_index = (kernel_params.path_index_array) ?
                             kernel_params.path_index_array[global_index] :
                             global_index;
  GPU_TIMER_START();
  integrator_intersect_subsurface(nullptr, path_index);
  GPU_TIMER_END();
}

extern "C" __global__ void __raygen__kernel_optix_integrator_intersect_volume_stack()
{
  const int global_index = optixGetLaunchIndex().x;
  const int path_index = (kernel_params.path_index_array) ?
                             kernel_params.path_index_array[global_index] :
                             global_index;
  GPU_TIMER_START();
  integrator_intersect_volume_stack(nullptr, path_index);
  GPU_TIMER_END();
}

extern "C" __global__ void __raygen__kernel_optix_integrator_intersect_dedicated_light()
{
  const int global_index = optixGetLaunchIndex().x;
  const int path_index = (kernel_params.path_index_array) ?
                             kernel_params.path_index_array[global_index] :
                             global_index;
  GPU_TIMER_START();
  integrator_intersect_dedicated_light(nullptr, path_index);
  GPU_TIMER_END();
}
