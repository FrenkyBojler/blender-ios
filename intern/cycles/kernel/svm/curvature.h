/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/integrator/path_state.h"

#include "kernel/bvh/bvh.h"

#include "kernel/sample/mapping.h"

#include "kernel/svm/node_types.h"
#include "kernel/svm/util.h"

CCL_NAMESPACE_BEGIN

#ifdef __SHADER_RAYTRACE__

#  ifdef __KERNEL_OPTIX__
extern "C" __device__ float __direct_callable__svm_node_curvature(
#  else
ccl_device float svm_curvature(
#  endif
    KernelGlobals kg,
    ConstIntegratorState state,
    ccl_private ShaderData *sd,
    float radius,
    const int num_samples,
    const int flags)
{
  /* Early out if no sampling needed. */
  if (radius <= 0.0f || num_samples < 1 || sd->object == OBJECT_NONE) {
    return 1.0f;
  }

  /* Can't ray-trace from shaders like displacement, before BVH exists. */
  if (kernel_data.bvh.bvh_layout == BVH_LAYOUT_NONE) {
    return 1.0f;
  }

  float3 N = sd->N;
  float3 T;
  float3 B;
  make_orthonormals(N, &T, &B);

  /* TODO: support ray-tracing in shadow shader evaluation? */
  RNGState rng_state;
  path_state_rng_load(state, &rng_state);

  int unoccluded = 0;
  for (int sample = 0; sample < num_samples; sample++) {
    const float2 rand_disk = path_branched_rng_2D(
        kg, &rng_state, sample, num_samples, PRNG_SURFACE_CONVEXITY);

    const float2 d = sample_uniform_disk(rand_disk);
    const float3 D = make_float3(d.x, d.y, safe_sqrtf(1.0f - dot(d, d)));

    /* Create ray. */
    Ray ray;
    ray.P = sd->P;
    ray.D = to_global(D, T, B, N);
    ray.tmin = 0.0f;
    ray.tmax = radius;
    ray.time = sd->time;
    ray.self.object = sd->object;
    ray.self.prim = sd->prim;
    ray.self.light_object = OBJECT_NONE;
    ray.self.light_prim = PRIM_NONE;
    ray.dP = differential_zero_compact();
    ray.dD = differential_zero_compact();

    if (flags & NODE_CURVATURE_ONLY_LOCAL) {
      if (!scene_intersect_local(kg, &ray, nullptr, sd->object, nullptr, 0)) {
        unoccluded++;
      }
    }
    else {
      if (!scene_intersect_shadow(kg, &ray, PATH_RAY_SHADOW_OPAQUE)) {
        unoccluded++;
      }
    }
  }

  return ((float)unoccluded) / num_samples;
}

template<uint node_feature_mask, typename ConstIntegratorGenericState>
#  if defined(__KERNEL_OPTIX__)
ccl_device_inline
#  else
ccl_device_noinline
#  endif
    void
    svm_node_curvature(KernelGlobals kg,
                       ConstIntegratorGenericState state,
                       ccl_private ShaderData *sd,
                       ccl_private float *ccl_restrict stack,
                       const ccl_global SVMNodeCurvature &ccl_restrict node)
{
  float curvature = 1.0f;

  IF_KERNEL_NODES_FEATURE(RAYTRACE)
  {
    float radius = stack_load(stack, node.radius);

#  ifdef __KERNEL_OPTIX__
    curvature = optixDirectCall<float>(0, kg, state, sd, radius, node.samples, node.flags);
#  else
    curvature = svm_curvature(kg, state, sd, radius, node.samples, node.flags);
#  endif
  }

  if (stack_valid(node.out_curvature_offset)) {
    stack_store_float(stack, node.out_curvature_offset, curvature);
  }
}

#endif /* __SHADER_RAYTRACE__ */

CCL_NAMESPACE_END
