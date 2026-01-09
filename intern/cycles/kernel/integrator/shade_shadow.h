/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/guiding.h"
#include "kernel/integrator/shade_volume.h"
#include "kernel/integrator/surface_shader.h"
#include "kernel/integrator/volume_stack.h"

#include "kernel/geom/shader_data.h"
#include "kernel/light/light.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline bool shadow_intersections_has_remaining(const uint num_hits)
{
  return num_hits >= INTEGRATOR_SHADOW_ISECT_SIZE;
}

#ifdef __TRANSPARENT_SHADOWS__
ccl_device_inline Spectrum integrate_transparent_surface_shadow(KernelGlobals kg,
                                                                IntegratorShadowState state,
                                                                const int hit)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_SURFACE);

  /* TODO: does aliasing like this break automatic SoA in CUDA?
   * Should we instead store closures separate from ShaderData?
   *
   * TODO: is it better to declare this outside the loop or keep it local
   * so the compiler can see there is no dependency between iterations? */
  ShaderDataTinyStorage shadow_sd_storage;
  ccl_private ShaderData *shadow_sd = AS_SHADER_DATA(&shadow_sd_storage);

  /* Setup shader data at surface. */
  Intersection isect ccl_optional_struct_init;
  integrator_state_read_shadow_isect(state, &isect, hit);

  Ray ray ccl_optional_struct_init;
  integrator_state_read_shadow_ray(state, &ray);

  shader_setup_from_ray(kg, shadow_sd, &ray, &isect);

  /* Evaluate shader. */
  if (!(shadow_sd->flag & SD_HAS_ONLY_VOLUME)) {
    surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_SHADOW>(
        kg, state, shadow_sd, nullptr, PATH_RAY_SHADOW);
  }
  else {
    INTEGRATOR_STATE_WRITE(state, shadow_path, volume_bounds_bounce) += 1;
  }

#  ifdef __VOLUME__
  /* Exit/enter volume. */
  volume_stack_enter_exit<true>(kg, state, shadow_sd);
#  endif

  /* Disable transparent shadows for ray portals */
  if (shadow_sd->flag & SD_RAY_PORTAL) {
    return zero_spectrum();
  }

  /* Compute transparency from closures. */
  return surface_shader_transparency(shadow_sd);
}

#  ifdef __VOLUME__
ccl_device_inline void integrate_transparent_volume_shadow(KernelGlobals kg,
                                                           IntegratorShadowState state,
                                                           const int hit,
                                                           const int num_recorded_hits,
                                                           ccl_private Spectrum *ccl_restrict
                                                               throughput)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_VOLUME);

  /* TODO: deduplicate with surface, or does it not matter for memory usage? */
  ShaderDataTinyStorage shadow_sd_storage;
  ccl_private ShaderData *shadow_sd = AS_SHADER_DATA(&shadow_sd_storage);

  /* Setup shader data. */
  Ray ray ccl_optional_struct_init;
  integrator_state_read_shadow_ray(state, &ray);
  ray.self.object = OBJECT_NONE;
  ray.self.prim = PRIM_NONE;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  /* Modify ray position and length to match current segment. */
  ray.tmin = (hit == 0) ? ray.tmin : INTEGRATOR_STATE_ARRAY(state, shadow_isect, hit - 1, t);
  ray.tmax = (hit < num_recorded_hits) ? INTEGRATOR_STATE_ARRAY(state, shadow_isect, hit, t) :
                                         ray.tmax;

  /* `object` is only needed for light tree with light linking, it is irrelevant for shadow. */
  shader_setup_from_volume(shadow_sd, &ray, OBJECT_NONE);

  if (kernel_data.integrator.volume_ray_marching) {
    const float step_size = volume_stack_step_size<true>(kg, state);
    volume_shadow_ray_marching(kg, state, &ray, shadow_sd, throughput, step_size);
  }
  else {
    volume_shadow_null_scattering(kg, state, &ray, shadow_sd, throughput);
  }
}
#  endif

ccl_device_inline bool integrate_transparent_shadow(KernelGlobals kg,
                                                    IntegratorShadowState state,
                                                    const uint num_hits)
{
  /* Accumulate shadow for transparent surfaces. */
  const uint num_recorded_hits = min(num_hits, (uint)INTEGRATOR_SHADOW_ISECT_SIZE);

  /* Plus one to account for world volume, which has no boundary to hit but casts shadows. */
  for (uint hit = 0; hit < num_recorded_hits + 1; hit++) {
    /* Volume shaders. */
    if (hit < num_recorded_hits || !shadow_intersections_has_remaining(num_hits)) {
#  ifdef __VOLUME__
      if (!integrator_state_shadow_volume_stack_is_empty(kg, state)) {
        Spectrum throughput = INTEGRATOR_STATE(state, shadow_path, throughput);
        integrate_transparent_volume_shadow(kg, state, hit, num_recorded_hits, &throughput);
        if (is_zero(throughput)) {
          return true;
        }

        INTEGRATOR_STATE_WRITE(state, shadow_path, throughput) = throughput;
      }
#  endif
    }

    /* Surface shaders. */
    if (hit < num_recorded_hits) {
      const Spectrum shadow = integrate_transparent_surface_shadow(kg, state, hit);
      const Spectrum throughput = INTEGRATOR_STATE(state, shadow_path, throughput) * shadow;
      if (is_zero(throughput)) {
        return true;
      }

      INTEGRATOR_STATE_WRITE(state, shadow_path, throughput) = throughput;
      INTEGRATOR_STATE_WRITE(state, shadow_path, transparent_bounce) += 1;
      INTEGRATOR_STATE_WRITE(state, shadow_path, rng_offset) += PRNG_BOUNCE_NUM;
    }

    if (INTEGRATOR_STATE(state, shadow_path, volume_bounds_bounce) > VOLUME_BOUNDS_MAX) {
      return true;
    }

    /* Note we do not need to check max_transparent_bounce here, the number
     * of intersections is already limited and made opaque in the
     * INTERSECT_SHADOW kernel. */
  }

  if (shadow_intersections_has_remaining(num_hits)) {
    /* There are more hits that we could not recorded due to memory usage,
     * adjust ray to intersect again from the last hit. */
    const float last_hit_t = INTEGRATOR_STATE_ARRAY(state, shadow_isect, num_recorded_hits - 1, t);
    INTEGRATOR_STATE_WRITE(state, shadow_ray, tmin) = intersection_t_offset(last_hit_t);
  }

  return false;
}
#endif /* __TRANSPARENT_SHADOWS__ */

ccl_device bool integrator_shade_direct_light(KernelGlobals kg, IntegratorShadowState state)
{
  /* Read intersection and ray. */
  Ray ray ccl_optional_struct_init;
  integrator_state_read_shadow_ray(state, &ray);
  integrator_state_read_shadow_ray_self(state, &ray);

  Intersection isect = {};
  isect.object = ray.self.light_object;
  isect.prim = ray.self.light_prim;
  isect.type = kernel_data_fetch(objects, isect.object).primitive_type;
  isect.t = ray.tmax;

  kernel_assert(isect.object != OBJECT_NONE);
  kernel_assert(isect.prim != PRIM_NONE);

  const int shader = (isect.type == PRIMITIVE_LAMP) ?
                         kernel_data_fetch(lights, isect.prim).shader_id :
                         intersection_get_shader(kg, &isect);

  float3 eval = zero_spectrum();
  if (!surface_shader_constant_emission(kg, shader, &eval)) {
    bool is_background = false;

    /* Setup shader data */
    ShaderDataCausticsStorage emission_sd_storage;
    ccl_private ShaderData *emission_sd = AS_SHADER_DATA(&emission_sd_storage);

    PROFILING_INIT_FOR_SHADER(kg, PROFILING_SHADE_LIGHT_SETUP);
    if (isect.type == PRIMITIVE_LAMP) {
      /* Lights. */
      const ccl_global KernelLight *klight = &kernel_data_fetch(lights, isect.prim);
      const LightType light_type = LightType(klight->type);

      if (light_type == LIGHT_BACKGROUND) {
        /* Backround light. */
        shader_setup_from_background(kg, emission_sd, ray.P, ray.D, ray.time);
        is_background = true;
      }
      else {
        /* Other light types.
         * Compute Ng and UV on demand so we don't have to store it in integrator state. */
        const float3 P = (ray.tmax == FLT_MAX) ? -ray.D : ray.P + ray.tmax * ray.D;
        float3 Ng = zero_float3();
        float2 uv = zero_float2();
        light_normal_uv_from_position(kg, klight, P, ray.D, Ng, uv);

        shader_setup_from_sample(kg,
                                 emission_sd,
                                 P,
                                 Ng,
                                 -ray.D,
                                 klight->shader_id,
                                 isect.object,
                                 isect.prim,
                                 uv.x,
                                 uv.y,
                                 ray.tmax,
                                 ray.time,
                                 false,
                                 true);
      }
    }
    else {
      /* Triangles.
       * Compute UV on demand so we don't have to store it in integrator state. */
      const float2 uv = triangle_light_uv(kg, isect.object, isect.prim, ray.time, ray.P, ray.D);
      isect.u = uv.x;
      isect.v = uv.y;

      shader_setup_from_ray(kg, emission_sd, &ray, &isect);
    }

    /* Evaluate shader. */
    PROFILING_SHADER(emission_sd->object, emission_sd->shader);
    PROFILING_EVENT(PROFILING_SHADE_LIGHT_EVAL);

    /* No proper path flag, we're evaluating this for all closures. that's
     * weak but we'd have to do multiple evaluations otherwise. */
    surface_shader_eval<KERNEL_FEATURE_NODE_MASK_SURFACE_LIGHT>(
        kg, state, emission_sd, nullptr, PATH_RAY_EMISSION);

    /* Evaluate emission closures. */
    eval = (is_background) ? surface_shader_background(emission_sd) :
                             surface_shader_emission(emission_sd);
  }

  /* Multiply in fixed light strength. */
  if (isect.type == PRIMITIVE_LAMP) {
    const ccl_global KernelLight *klight = &kernel_data_fetch(lights, isect.prim);
    eval *= rgb_to_spectrum(
        make_float3(klight->strength[0], klight->strength[1], klight->strength[2]));
  }

  /* Update throughput. */
  INTEGRATOR_STATE(state, shadow_path, throughput) *= eval;
  if (kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING) {
    INTEGRATOR_STATE(state, shadow_path, unlit_throughput) *= eval;
  }

  return true;
}

ccl_device void integrator_shade_shadow(KernelGlobals kg,
                                        IntegratorShadowState state,
                                        ccl_global float *ccl_restrict render_buffer)
{
  PROFILING_INIT(kg, PROFILING_SHADE_SHADOW_SETUP);
  const uint num_hits = INTEGRATOR_STATE(state, shadow_path, num_hits);

#ifdef __TRANSPARENT_SHADOWS__
  /* Evaluate transparent shadows. */
  const bool opaque = integrate_transparent_shadow(kg, state, num_hits);
  if (opaque) {
    integrator_shadow_path_terminate(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
    return;
  }
#endif

  if (shadow_intersections_has_remaining(num_hits)) {
    /* More intersections to find, continue shadow ray. */
    integrator_shadow_path_next(
        state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW);
    return;
  }

  const uint32_t path_flag = INTEGRATOR_STATE(state, shadow_path, flag);
  bool have_light = true;
  if (!(path_flag & PATH_RAY_SHADOW_FOR_AO)) {
    have_light = integrator_shade_direct_light(kg, state);
    if (have_light) {
      guiding_record_direct_light(kg, state);
    }
  }

  if (have_light) {
    film_write_direct_light(kg, state, render_buffer);
  }
  integrator_shadow_path_terminate(state, DEVICE_KERNEL_INTEGRATOR_SHADE_SHADOW);
}

CCL_NAMESPACE_END
