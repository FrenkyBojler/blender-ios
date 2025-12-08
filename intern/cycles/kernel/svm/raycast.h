/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/integrator/path_state.h"

#include "kernel/bvh/bvh.h"

#include "kernel/sample/mapping.h"

#include "kernel/svm/util.h"

CCL_NAMESPACE_BEGIN

#ifdef __SHADER_RAYTRACE__

#  ifdef __KERNEL_OPTIX__
extern "C" __device__ float __direct_callable__svm_node_raycast(
#  else
ccl_device float svm_raycast(
#  endif
    KernelGlobals kg,
    ConstIntegratorState /*state*/,
    ccl_private ShaderData *sd,
    float3 position,
    float3 direction,
    float distance,
    bool only_local)
{
  /* Early out if no sampling needed. */
  if (distance <= 0.0f || sd->object == OBJECT_NONE) {
    return -1.0f;
  }

  /* Can't ray-trace from shaders like displacement, before BVH exists. */
  if (kernel_data.bvh.bvh_layout == BVH_LAYOUT_NONE) {
    return -1.0f;
  }

  const bool avoid_self_intersection = isequal(position, sd->P);

  /* Create ray. */
  Ray ray;
  ray.P = position;
  ray.D = direction;
  ray.tmin = 0.0f;
  ray.tmax = distance;
  ray.time = sd->time;
  ray.self.object = avoid_self_intersection ? sd->object : OBJECT_NONE;
  ray.self.prim = avoid_self_intersection ? sd->prim : PRIM_NONE;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  ray.dP = differential_zero_compact();
  ray.dD = differential_zero_compact();

  if (only_local) {
    LocalIntersection isect;
    scene_intersect_local(kg, &ray, &isect, sd->object, nullptr, 1);
    if (isect.num_hits == 0) {
      return -1.0f;
    }
    return isect.hits[0].t;
  }
  else {
    Intersection isect;
    /* Ray-trace, leaving out shadow opaque to avoid early exit. */
    const uint visibility = PATH_RAY_ALL_VISIBILITY - PATH_RAY_SHADOW_OPAQUE;
    if (!scene_intersect_material_raycast(kg, &ray, visibility, &isect)) {
      return -1.0f;
    }
    return isect.t;
  }
}

template<uint node_feature_mask, typename ConstIntegratorGenericState>
#  if defined(__KERNEL_OPTIX__)
ccl_device_inline
#  else
ccl_device_noinline
#  endif
    void
    svm_node_raycast(KernelGlobals kg,
                     ConstIntegratorGenericState state,
                     ccl_private ShaderData *sd,
                     ccl_private float *stack,
                     const uint4 node)
{
  uint position_offset;
  uint direction_offset;
  uint distance_offset;
  uint is_hit_offset;
  svm_unpack_node_uchar4(
      node.y, &position_offset, &direction_offset, &distance_offset, &is_hit_offset);

  uint hit_position_offset;
  uint hit_distance_offset;
  uint only_local;
  svm_unpack_node_uchar3(node.z, &hit_position_offset, &hit_distance_offset, &only_local);

  float distance = stack_load_float_default(stack, distance_offset, 0.0f);

  float is_hit = 0.0;
  float3 hit_position = make_float3(0.0f);
  float hit_distance = distance;

  IF_KERNEL_NODES_FEATURE(RAYTRACE)
  {
    float3 position = stack_load_float3_default(stack, position_offset, sd->P);
    float3 direction = stack_load_float3_default(stack, direction_offset, sd->N);
#  ifdef __KERNEL_OPTIX__
    float result = optixDirectCall<float>(
        2, kg, state, sd, position, direction, distance, only_local);
#  else
    float result = svm_raycast(kg, state, sd, position, direction, distance, only_local);
#  endif

    if (result >= 0.0f) {
      is_hit = 1.0f;
      hit_distance = result;
      hit_position = position + direction * hit_distance;
    }
  }

  if (stack_valid(is_hit_offset)) {
    stack_store_float(stack, is_hit_offset, is_hit);
  }
  if (stack_valid(hit_position_offset)) {
    stack_store_float3(stack, hit_position_offset, hit_position);
  }
  if (stack_valid(hit_distance_offset)) {
    stack_store_float(stack, hit_distance_offset, hit_distance);
  }
}

#endif /* __SHADER_RAYTRACE__ */

CCL_NAMESPACE_END
