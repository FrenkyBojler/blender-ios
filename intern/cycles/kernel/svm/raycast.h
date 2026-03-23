/* SPDX-FileCopyrightText: 2011-2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/integrator/path_state.h"

#include "kernel/bvh/bvh.h"

#include "kernel/sample/mapping.h"

#include "kernel/svm/util.h"

#include "kernel/geom/shader_data.h"

CCL_NAMESPACE_BEGIN

#ifdef __SHADER_RAYTRACE__

struct RaycastResult {
  float distance;
  float3 normal;
  bool self_hit;
};

ccl_device RaycastResult svm_raycast(KernelGlobals kg,
                                     ConstIntegratorState /*state*/,
                                     ccl_private ShaderData *sd,
                                     float3 position,
                                     float3 direction,
                                     float3 offset,
                                     float distance,
                                     bool only_local,
                                     NodeRaycastMode mode,
                                     float bump_filter_width)
{
  RaycastResult result;
  result.distance = -1.0f;
  result.normal = make_float3(0.0f);
  result.self_hit = false;

  /* Early out if no sampling needed. */
  if (distance <= 0.0f || sd->object == OBJECT_NONE) {
    return result;
  }

  /* Can't ray-trace from shaders like displacement, before BVH exists. */
  if (kernel_data.bvh.bvh_layout == BVH_LAYOUT_NONE) {
    return result;
  }

  if (mode == NODE_RAYCAST_MODE_OFFSET) {
    float3 I = shading_incoming<float3>(sd);
    position = sd->P + I * sd->ray_length;
    distance += sd->ray_length;
    float3 camera_up = normalize(
        transform_direction_transposed(&kernel_data.cam.worldtocamera, make_float3(0, 1, 0)));
    float3 ray_tangent = normalize(cross(-I, camera_up));
    float3 ray_bitangent = normalize(cross(I, ray_tangent));
    float3 target = sd->P + (ray_tangent * offset.x) + (ray_bitangent * offset.y);
    direction = normalize(target - position);
  }

  float tmin = 0.0f;
  bool avoid_self_intersection = false;
  if (bump_filter_width > 0.0f) {
    /* If evaluating for bump mapping at a shifted position, increase min
     * distance by slightly more than the shift distance to avoid self
     * intersections. */
    tmin = bump_filter_width * sd->dP * 1.1f;
  }
  else {
    avoid_self_intersection = isequal(position, sd->P);
  }

  /* Create ray. */
  Ray ray;
  ray.P = position;
  ray.D = direction;
  ray.tmin = tmin;
  ray.tmax = distance;
  ray.time = sd->time;
  ray.self.object = avoid_self_intersection ? sd->object : OBJECT_NONE;
  ray.self.prim = avoid_self_intersection ? sd->prim : PRIM_NONE;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  ray.dP = differential_zero_compact();
  ray.dD = differential_zero_compact();

  Intersection isect;
  if (only_local) {
    LocalIntersection local_isect;
    scene_intersect_local(kg, &ray, &local_isect, sd->object, nullptr, 1);
    if (local_isect.num_hits == 0) {
      return result;
    }
    isect = local_isect.hits[0];
  }
  else {
    /* Ray-trace, leaving out shadow opaque to avoid early exit. */
    const uint visibility = PATH_RAY_ALL_VISIBILITY - PATH_RAY_SHADOW_OPAQUE;
    if (!scene_intersect(kg, &ray, visibility, &isect)) {
      return result;
    }
  }

  result.distance = isect.t;
  result.self_hit = isect.object == sd->object;

  ShaderDataTinyStorage hit_sd_storage;
  ccl_private ShaderData *hit_sd = AS_SHADER_DATA(&hit_sd_storage);
  shader_setup_from_ray(kg, hit_sd, &ray, &isect);
  result.normal = hit_sd->N;

  return result;
}

template<uint node_feature_mask, typename ConstIntegratorGenericState>
#  if defined(__KERNEL_OPTIX__)
ccl_device_inline
#  else
ccl_device_noinline
#  endif
    int
    svm_node_raycast(KernelGlobals kg,
                     ConstIntegratorGenericState state,
                     ccl_private ShaderData *sd,
                     ccl_private float *stack,
                     const uint4 node,
                     int offset)
{
  uint position_offset;
  uint direction_offset;
  uint offset_offset;
  uint distance_offset;
  svm_unpack_node_uchar4(
      node.y, &position_offset, &direction_offset, &offset_offset, &distance_offset);

  uint is_hit_offset;
  uint is_self_hit_offset;
  uint hit_distance_offset;
  uint hit_position_offset;
  svm_unpack_node_uchar4(
      node.z, &is_hit_offset, &is_self_hit_offset, &hit_distance_offset, &hit_position_offset);

  uint hit_normal_offset;
  uint only_local;
  uint mode;
  svm_unpack_node_uchar3(node.w, &hit_normal_offset, &only_local, &mode);

  float distance = stack_load_float_default(stack, distance_offset, 0.0f);

  float is_hit = 0.0f;
  float is_self_hit = 0.0f;
  float hit_distance = distance;
  float3 hit_position = make_float3(0.0f);
  float3 hit_normal = make_float3(0.0f);

  uint4 data_node = read_node(kg, &offset);

  IF_KERNEL_NODES_FEATURE(RAYTRACE)
  {
    const float bump_filter_width = __uint_as_float(data_node.x);

    float3 position = stack_load_float3(stack, position_offset);
    float3 direction = stack_load_float3(stack, direction_offset);
    float3 offset = stack_load_float3(stack, offset_offset);
    RaycastResult result = svm_raycast(kg,
                                       state,
                                       sd,
                                       position,
                                       direction,
                                       offset,
                                       distance,
                                       only_local,
                                       NodeRaycastMode(mode),
                                       bump_filter_width);

    if (result.distance >= 0.0f) {
      is_hit = 1.0f;
      is_self_hit = result.self_hit ? 1.0f : 0.0f;
      hit_distance = result.distance;
      hit_position = position + direction * hit_distance;
      hit_normal = result.normal;
    }
  }

  if (stack_valid(is_hit_offset)) {
    stack_store_float(stack, is_hit_offset, is_hit);
  }
  if (stack_valid(is_self_hit_offset)) {
    stack_store_float(stack, is_self_hit_offset, is_self_hit);
  }
  if (stack_valid(hit_distance_offset)) {
    stack_store_float(stack, hit_distance_offset, hit_distance);
  }
  if (stack_valid(hit_position_offset)) {
    stack_store_float3(stack, hit_position_offset, hit_position);
  }
  if (stack_valid(hit_normal_offset)) {
    stack_store_float3(stack, hit_normal_offset, hit_normal);
  }

  return offset;
}

#endif /* __SHADER_RAYTRACE__ */

CCL_NAMESPACE_END
