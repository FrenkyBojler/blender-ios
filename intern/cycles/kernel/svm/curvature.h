/* SPDX-FileCopyrightText: 2011-2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/globals.h"

#include "kernel/integrator/path_state.h"

#include "kernel/bvh/bvh.h"

#include "kernel/sample/mapping.h"

#include "kernel/svm/node_types.h"
#include "kernel/svm/util.h"

#include "kernel/geom/shader_data.h"

CCL_NAMESPACE_BEGIN

#ifdef __SHADER_RAYTRACE__

struct CurvaturePass {
  float3 Ng;
  float3 N;
  int dimension;
  float *sum_angles;
  float *sum_weights;
};

/* Raycast to sample angle deviation between input normal and hit normal. */
ccl_device_inline bool svm_curvature_raycast_angle(KernelGlobals kg,
                                                   ccl_private ShaderData *sd,
                                                   const ccl_private CurvaturePass *pass,
                                                   const float3 D,
                                                   const float radius,
                                                   const bool only_local,
                                                   float &angle,
                                                   float3 &hit_P)
{
  /* Create ray. */
  Ray ray;
  ray.P = ray_offset(sd->P, pass->Ng);
  ray.D = D;
  ray.tmin = 0.0f;
  /* Rays are fired across a hemisphere but hits are limited to a disk of the given radius.
   * tmax is radius * sqrt(2) so diagonal rays reach the disk boundary. */
  ray.tmax = radius * M_SQRT2_F;
  ray.time = sd->time;
  ray.self.object = sd->object;
  ray.self.prim = sd->prim;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  ray.dP = differential_zero_compact();
  ray.dD = differential_zero_compact();

  Intersection isect;
  if (only_local) {
    LocalIntersection local_isect;
    scene_intersect_local(kg, &ray, &local_isect, sd->object, nullptr, 1);
    if (local_isect.num_hits == 0) {
      return false;
    }
    isect = local_isect.hits[0];
  }
  else {
    /* Ray-trace, leaving out shadow opaque to avoid early exit. */
    const PathRayVisibility visibility = PATH_RAY_VISIBILITY_ALL &
                                         ~PATH_RAY_VISIBILITY_SHADOW_OPAQUE;
    if (!scene_intersect(kg, &ray, visibility, &isect)) {
      return false;
    }
  }

  ShaderDataTinyStorage hit_sd_storage;
  ccl_private ShaderData &hit_sd = *AS_SHADER_DATA(&hit_sd_storage);
  shader_setup_from_ray(kg, &hit_sd, &ray, &isect);

  /* Skip back faces. */
  const bool is_mesh = sd->type & PRIMITIVE_TRIANGLE;
  const bool is_backfacing = hit_sd.flag & SD_BACKFACING;
  if (is_mesh && is_backfacing == (pass->dimension == PRNG_SURFACE_CONCAVITY)) {
    return false;
  }

  hit_P = hit_sd.P;
  angle = vector_angle(pass->N, hit_sd.N);
  return true;
}

/* Curvature shader measuring the average angular difference
 * between nearby surface normals sampled by two opposite hemispheres.
 * Influence falls off with distance projected onto a 2D disk of the given radius. */

#  ifdef __KERNEL_OPTIX__
extern "C" __device__ float3 __direct_callable__svm_node_curvature(
#  else
ccl_device float3 svm_curvature(
#  endif
    KernelGlobals kg,
    ConstIntegratorState state,
    ccl_private ShaderData *sd,
    const float radius,
    const int num_samples,
    const int flags)
{
  /* Early out if no sampling needed. */
  if (radius <= 0.0f || num_samples < 1 || sd->object == OBJECT_NONE) {
    return make_float3(0.5f, 0.0f, 0.0f);
  }

  /* Can't ray-trace from shaders like displacement, before BVH exists. */
  if (kernel_data.bvh.bvh_layout == BVH_LAYOUT_NONE) {
    return make_float3(0.5f, 0.0f, 0.0f);
  }

  float3 T;
  float3 B;
  make_orthonormals(sd->Ng, &T, &B);

  /* TODO: support ray-tracing in shadow shader evaluation? */
  RNGState rng_state;
  path_state_rng_load(state, &rng_state);

  float sum_convexity = 0.0f;
  float sum_concavity = 0.0f;
  float sum_weights_convexity = 0.0f;
  float sum_weights_concavity = 0.0f;

  ccl_private CurvaturePass passes[2] = {
      {-sd->Ng, -sd->N, PRNG_SURFACE_CONVEXITY, &sum_convexity, &sum_weights_convexity},
      {sd->Ng, sd->N, PRNG_SURFACE_CONCAVITY, &sum_concavity, &sum_weights_concavity},
  };

  /* Sample opposite hemispheres for convexity and concavity. */
  for (int sample = 0; sample < num_samples; sample++) {
    for (int i = 0; i < 2; i++) {
      ccl_private CurvaturePass *pass = &passes[i];

      const float2 rand = path_branched_rng_2D(
          kg, &rng_state, sample, num_samples, pass->dimension);

      float3 D;
      float pdf;
      sample_uniform_hemisphere(pass->Ng, rand, &D, &pdf);

      float angle;
      float3 hit_P;
      const bool only_local = flags & NODE_CURVATURE_ONLY_LOCAL;

      if (svm_curvature_raycast_angle(kg, sd, pass, D, radius, only_local, angle, hit_P)) {
        const float weight = 1.0f / pdf;

        /* Project hit into disk plane to measure lateral distance for falloff. */
        const float3 offset_P = hit_P - sd->P;
        const float dist_proj = len(to_local(offset_P, T, B));
        const float dist_factor = dist_proj / radius;

        if (dist_factor <= 1.0f) {
          const float falloff = 1.0f - dist_factor;
          *pass->sum_angles += angle * falloff * weight;
          *pass->sum_weights += weight;
        }
      }
    }
  }

  float curvature;
  float convexity;
  float concavity;

  float sum_weights = sum_weights_convexity + sum_weights_concavity;
  curvature = safe_divide(sum_convexity - sum_concavity, sum_weights);
  convexity = max(0.0f, curvature);
  concavity = max(0.0f, -curvature);

  curvature = (curvature + M_PI_F) * M_1_2PI_F;

  return make_float3(curvature, convexity, concavity);
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
  float3 result = make_float3(0.5f, 0.0f, 0.0f);

  IF_KERNEL_NODES_FEATURE(RAYTRACE)
  {
    float radius = stack_load(stack, node.radius);

#  ifdef __KERNEL_OPTIX__
    result = optixDirectCall<float3>(2, kg, state, sd, radius, node.samples, node.flags);
#  else
    result = svm_curvature(kg, state, sd, radius, node.samples, node.flags);
#  endif
  }

  float curvature = result.x;
  float convexity = result.y;
  float concavity = result.z;

  if (stack_valid(node.out_curvature_offset)) {
    stack_store_float(stack, node.out_curvature_offset, curvature);
  }
  if (stack_valid(node.out_convexity_offset)) {
    stack_store_float(stack, node.out_convexity_offset, convexity);
  }
  if (stack_valid(node.out_concavity_offset)) {
    stack_store_float(stack, node.out_concavity_offset, concavity);
  }
}

#endif /* __SHADER_RAYTRACE__ */

CCL_NAMESPACE_END
