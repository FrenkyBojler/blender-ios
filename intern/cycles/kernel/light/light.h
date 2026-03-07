/* SPDX-FileCopyrightText: 2010-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/geom/object.h"
#include "kernel/globals.h"

#include "kernel/integrator/state.h"

#include "kernel/light/area.h"
#include "kernel/light/background.h"
#include "kernel/light/distant.h"
#include "kernel/light/point.h"
#include "kernel/light/spot.h"
#include "kernel/light/triangle.h"
#include "kernel/types.h"

CCL_NAMESPACE_BEGIN

/* Light info. */

ccl_device_inline bool light_select_reached_max_bounces(KernelGlobals kg,
                                                        const int prim,
                                                        const int bounce)
{
  return (bounce > kernel_data_fetch(light_geom, prim).max_bounces);
}

/* Light linking. */

ccl_device_inline int light_link_receiver_nee(KernelGlobals kg, const ccl_private ShaderData *sd)
{
#ifdef __LIGHT_LINKING__
  if (!(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_LINKING)) {
    return OBJECT_NONE;
  }

  return sd->object;
#else
  return OBJECT_NONE;
#endif
}

ccl_device_inline int light_link_receiver_forward(KernelGlobals kg, IntegratorState state)
{
#ifdef __LIGHT_LINKING__
  if (!(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_LINKING)) {
    return OBJECT_NONE;
  }

  return INTEGRATOR_STATE(state, path, mis_ray_object);
#else
  return OBJECT_NONE;
#endif
}

ccl_device_inline bool light_link_object_match(KernelGlobals kg,
                                               const int receiver,
                                               const int emitter)
{
#ifdef __LIGHT_LINKING__
  if (!(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_LINKING)) {
    return true;
  }

  kernel_assert(emitter != OBJECT_NONE);
  kernel_assert(receiver != OBJECT_NONE);

  const uint64_t set_membership = kernel_data_fetch(objects, emitter).light_set_membership;
  const uint receiver_set = kernel_data_fetch(objects, receiver).receiver_light_set;
  return ((uint64_t(1) << uint64_t(receiver_set)) & set_membership) != 0;
#else
  return true;
#endif
}

ccl_device_inline const ccl_global KernelLight *get_light_from_object_id(KernelGlobals kg,
                                                                         const int object_id)
{
  const ccl_global KernelObject *kobject = &kernel_data_fetch(objects, object_id);
  return &kernel_data_fetch(lights, kobject->light_id);
}

/* Sample point on an individual light. */

template<bool in_volume_segment>
ccl_device_inline bool light_sample(KernelGlobals kg,
                                    const int object,
                                    const float2 rand,
                                    const float3 P,
                                    const float3 N,
                                    const int shader_flags,
                                    const uint32_t path_flag,
                                    ccl_private LightSample *ls)
{
  const ccl_global KernelLight *klight = get_light_from_object_id(kg, object);
  if (path_flag & PATH_RAY_SHADOW_CATCHER_PASS) {
    if (klight->shader_id_and_flags & SHADER_EXCLUDE_SHADOW_CATCHER) {
      return false;
    }
  }

  const LightType type = (LightType)klight->type;
  ls->type = type;
  ls->shader_id_and_flags = klight->shader_id_and_flags;
  ls->object = object;
  ls->prim = klight->prim;
  ls->group = object_lightgroup(kg, ls->object);

  if (in_volume_segment && (type == LIGHT_DISTANT || type == LIGHT_BACKGROUND)) {
    /* Distant lights in a volume get a dummy sample, position will not actually
     * be used in that case. Only when sampling from a specific scatter position
     * do we actually need to evaluate these. */
    ls->P = zero_float3();
    ls->Ng = zero_float3();
    ls->D = zero_float3();
    ls->pdf = 1.0f;
    ls->eval_fac = 0.0f;
    ls->t = FLT_MAX;
    return true;
  }

  if (type == LIGHT_DISTANT) {
    if (!distant_light_sample(klight, rand, ls)) {
      return false;
    }
  }
  else if (type == LIGHT_BACKGROUND) {
    /* infinite area light (e.g. light dome or env light) */
    const float3 D = -background_light_sample(kg, P, rand, &ls->pdf);

    ls->P = D;
    ls->Ng = D;
    ls->D = -D;
    ls->t = FLT_MAX;
    ls->eval_fac = 1.0f;
  }
  else if (type == LIGHT_SPOT) {
    if (!spot_light_sample<in_volume_segment>(kg, klight, rand, P, N, shader_flags, ls)) {
      return false;
    }
  }
  else if (type == LIGHT_POINT) {
    if (!point_light_sample(klight, rand, P, N, shader_flags, ls)) {
      return false;
    }
  }
  else {
    /* area light */
    if (!area_light_sample<in_volume_segment>(klight, rand, P, ls)) {
      return false;
    }
  }

  return in_volume_segment || (ls->pdf > 0.0f);
}

/* Sample a point on the chosen emitter. */

template<bool in_volume_segment>
ccl_device_noinline bool light_sample(KernelGlobals kg,
                                      const float3 rand_light,
                                      const float time,
                                      const float3 P,
                                      const float3 N,
                                      const int object_receiver,
                                      const int shader_flags,
                                      const int bounce,
                                      const uint32_t path_flag,
                                      ccl_private LightSample *ls)
{
  /* The first two dimensions of the Sobol sequence have better stratification, use them to sample
   * position on the light. */
  const float2 rand = make_float2(rand_light);

  int prim;
  int visibility_flag;
  int object_id;
#ifdef __LIGHT_TREE__
  if (kernel_data.integrator.use_light_tree) {
    const ccl_global KernelLightTreeEmitter *kemitter = &kernel_data_fetch(light_tree_emitters,
                                                                           ls->emitter_id);
    prim = kemitter->light.prim;
    visibility_flag = kemitter->visibility_flag;
    object_id = (prim >= 0) ? ls->object : kemitter->object_id;
  }
  else
#endif
  {
    const ccl_global KernelLightDistribution *kdistribution = &kernel_data_fetch(
        light_distribution, ls->emitter_id);
    prim = kdistribution->prim;
    object_id = kdistribution->object_id;
    visibility_flag = kdistribution->visibility_flag;
  }

  if (!light_link_object_match(kg, object_receiver, object_id)) {
    return false;
  }

  if (prim >= 0) {
    /* Mesh light. */

    /* Exclude synthetic meshes from shadow catcher pass. */
    if ((path_flag & PATH_RAY_SHADOW_CATCHER_PASS) &&
        !(kernel_data_fetch(object_flag, object_id) & SD_OBJECT_SHADOW_CATCHER))
    {
      return false;
    }

    if (!triangle_light_sample<in_volume_segment>(kg, prim, object_id, rand, time, ls, P)) {
      return false;
    }
    ls->shader_id_and_flags |= visibility_flag;
  }
  else {
    if (UNLIKELY(light_select_reached_max_bounces(kg, ~prim, bounce))) {
      return false;
    }

    if (!light_sample<in_volume_segment>(kg, object_id, rand, P, N, shader_flags, path_flag, ls)) {
      return false;
    }
  }

  ls->pdf *= ls->pdf_selection;
  return in_volume_segment || (ls->pdf > 0.0f);
}

ccl_device_inline bool lights_intersect(KernelGlobals kg,
                                        ccl_private Intersection *ccl_restrict isect,
                                        const float3 P,
                                        const float3 dir,
                                        const float tmin,
                                        const int object,
                                        const int prim)
{
  const ccl_global KernelLightGeom *klight = &kernel_data_fetch(light_geom, prim);

  const LightType type = (LightType)klight->type;
  float t = 0.0f;

  Ray ray = {P, dir, tmin, isect->t};

  if (type == LIGHT_SPOT) {
    if (!spot_light_intersect(klight, &ray, &t)) {
      return false;
    }
  }
  else if (type == LIGHT_POINT) {
    if (!point_light_intersect(klight, &ray, &t)) {
      return false;
    }
  }
  else if (type == LIGHT_AREA) {
    if (!area_light_intersect(klight, &ray, &t)) {
      return false;
    }
  }
  else {
    /* No distant lights in the bvh. */
    return false;
  }

  isect->object = object;
  isect->prim = prim;
  isect->type = PRIMITIVE_LAMP;
  isect->t = t;
  return true;
}

/* Setup light sample from intersection. */

ccl_device LightEval
light_eval_from_intersection(KernelGlobals kg,
                             const ccl_private Intersection *ccl_restrict isect,
                             const float3 ray_P,
                             const float3 ray_D,
                             const float3 N,
                             const uint32_t path_flag)
{
  const ccl_global KernelLight *klight = get_light_from_object_id(kg, isect->object);
  const LightType type = (LightType)klight->type;

  if (type == LIGHT_SPOT) {
    return spot_light_eval_from_intersection(kg, klight, ray_P, ray_D, isect->t, N, path_flag);
  }
  if (type == LIGHT_POINT) {
    return point_light_eval_from_intersection(klight, ray_P, ray_D, isect->t, N, path_flag);
  }
  if (type == LIGHT_AREA) {
    return area_light_eval_from_intersection(klight, ray_P, ray_D, isect->t);
  }

  kernel_assert(!"Invalid lamp type in light_eval_from_intersection");
  return LightEval{};
}

/* Get light coordinates from position on light. */
ccl_device void light_normal_uv_from_position(KernelGlobals kg,
                                              const int object_id,
                                              const float3 P,
                                              const float3 D,
                                              ccl_private float3 &Ng,
                                              ccl_private float2 &uv)
{
  const ccl_global KernelLight *klight = get_light_from_object_id(kg, object_id);
  const LightType type = (LightType)klight->type;

  if (type == LIGHT_SPOT) {
    Ng = (klight->spot.is_sphere) ? normalize(P - klight->co) : -D;
    const float3 local_ray = spot_light_to_local(kg, klight, -D);
    uv = spot_light_uv(local_ray, klight->spot.half_cot_half_spot_angle);
  }
  else if (type == LIGHT_POINT) {
    Ng = (klight->spot.is_sphere) ? normalize(P - klight->co) : -D;
    uv = point_light_uv(kg, klight, Ng);
  }
  else if (type == LIGHT_AREA) {
    Ng = klight->area.dir;
    uv = area_light_uv(klight, P);
  }
  else if (type == LIGHT_DISTANT) {
    Ng = -D;
    uv = distant_light_uv(kg, klight, D);
  }
  else {
    kernel_assert(0);
  }
}

CCL_NAMESPACE_END
