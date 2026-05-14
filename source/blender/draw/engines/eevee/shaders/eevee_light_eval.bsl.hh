/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_bxdf_lib.glsl"
#include "eevee_closure_lib.glsl"
#include "eevee_light_iter.bsl.hh"
#include "eevee_light_lib.glsl"
#include "eevee_shadow.bsl.hh"
#include "eevee_shadow_tracing.bsl.hh"
#include "eevee_thickness_lib.bsl.hh"
#include "gpu_shader_codegen_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

#if !defined(SRT_CONSTANT_light_closure_eval_count)
#  define SRT_CONSTANT_light_closure_eval_count 1
#  define SKIP_LIGHT_EVAL
#endif

#ifdef GLSL_CPP_STUBS
#  define LIGHT_STACK_SIZE 3
#elif SRT_CONSTANT_light_closure_eval_count == 0
#  define LIGHT_STACK_SIZE 1 /* Avoid compilation error. */
#else
#  define LIGHT_STACK_SIZE SRT_CONSTANT_light_closure_eval_count
#endif

namespace eevee::light {

struct ClosureStack {
  /* NOTE: This is wrapped into a struct to avoid array shenanigans on MSL. */
  ClosureLight cl[LIGHT_STACK_SIZE];
};

ClosureLight closure_get(ClosureStack stack, uchar index)
{
  switch (index) {
    case 0:
      return stack.cl[0];
#if SRT_CONSTANT_light_closure_eval_count > 1
    case 1:
      return stack.cl[1];
#endif
#if SRT_CONSTANT_light_closure_eval_count > 2
    case 2:
      return stack.cl[2];
#endif
#if SRT_CONSTANT_light_closure_eval_count > 3
#  error
#endif
  }
  ClosureLight closure_null;
  return closure_null;
}

void closure_set(ClosureStack &stack, uchar index, ClosureLight cl_light)
{
  switch (index) {
    case 0:
      stack.cl[0] = cl_light;
      break;
#if SRT_CONSTANT_light_closure_eval_count > 1
    case 1:
      stack.cl[1] = cl_light;
      break;
#endif
#if SRT_CONSTANT_light_closure_eval_count > 2
    case 2:
      stack.cl[2] = cl_light;
      break;
#endif
#if SRT_CONSTANT_light_closure_eval_count > 3
#  error
#endif
  }
}

float light_power_get(LightData light, LightingType type)
{
  /* Mask anything above 3. See LIGHT_TRANSLUCENT_WITH_THICKNESS. */
  return light.power[type & 3u];
}

bool light_linking_affects_receiver(uint2 light_set_membership, uchar receiver_light_set)
{
  return bitmask64_test(light_set_membership, receiver_light_set);
}

void light_eval_single_closure(
    LightData light, LightVector lv, ClosureLight &cl, float3 V, float attenuation, float shadow)
{
  attenuation *= light_power_get(light, cl.type);
  if (attenuation < 1e-30f) {
    return;
  }
  auto &util_tx = sampler_get(eevee_utility_texture, utility_tx);
  float ltc_result = light_ltc(util_tx, light, cl.N, V, lv, cl.ltc_mat);
  float3 out_radiance = light.color * ltc_result;
  float visibility = shadow * attenuation;
  cl.light_shadowed += visibility * out_radiance;
  cl.light_unshadowed += attenuation * out_radiance;
}

template<bool is_transmission> struct LightEvalCtx {
  ClosureStack stack;

  float3 P;
  float3 Ng;
  float3 V;
  Thickness thickness;
  uchar receiver_light_set;
  float terminator_normal_offset;
  float terminator_geometry_offset;

  void light_eval_single([[resource_table]] ShadowRenderData &srd,
                         LightData light,
                         const bool is_directional)
  {
    if (!light_linking_affects_receiver(light.light_set_membership, receiver_light_set)) {
      return;
    }

#if defined(SPECIALIZED_SHADOW_PARAMS) || defined(SRT_CONSTANT_shadow_ray_count)
    int ray_count = shadow_ray_count;
    int ray_step_count = shadow_ray_step_count;
#else
    int ray_count = uniform_buf.shadow.ray_count;
    int ray_step_count = uniform_buf.shadow.step_count;
#endif

    LightVector lv = light_vector_get(light, is_directional, P);

    /* TODO(fclem): Get rid of this special case. */
    bool is_translucent_with_thickness = is_transmission &&
                                         (stack.cl[0].type == LIGHT_TRANSLUCENT_WITH_THICKNESS);

    float attenuation = light_attenuation_surface(light, is_directional, lv);

    if (!is_translucent_with_thickness) {
      /* Only do attenuation for this case, since we integrate the whole sphere for translucency.
       * Moreover, stack.cl[0].N is overwritten for is_translucent_with_thickness. */
      attenuation *= light_attenuation_facing(
          light, lv.L, lv.dist, stack.cl[0].N, is_transmission);
    }

    if (attenuation < LIGHT_ATTENUATION_THRESHOLD) {
      return;
    }

    float shadow = 1.0f;
    if (light.tilemap_index != LIGHT_NO_SHADOW) {
      shadow = shadow_eval(srd,
                           light,
                           is_directional,
                           is_transmission,
                           is_translucent_with_thickness,
                           thickness,
                           P,
                           Ng,
                           stack.cl[0].N,
                           terminator_normal_offset,
                           terminator_geometry_offset,
                           ray_count,
                           ray_step_count);
    }

    if (is_translucent_with_thickness) {
      /* This makes the LTC compute the solid angle of the light (still with the cosine term
       * applied but that still works great enough in practice). */
      stack.cl[0].N = lv.L;
      /* Adjust power because of the second lambertian distribution. */
      attenuation *= M_1_PI;
    }

    light_eval_single_closure(light, lv, stack.cl[0], V, attenuation, shadow);
    if (!is_transmission) {
#if SRT_CONSTANT_light_closure_eval_count > 1
      light_eval_single_closure(light, lv, stack.cl[1], V, attenuation, shadow);
#endif
#if SRT_CONSTANT_light_closure_eval_count > 2
      light_eval_single_closure(light, lv, stack.cl[2], V, attenuation, shadow);
#endif
#if SRT_CONSTANT_light_closure_eval_count > 3
#  error
#endif
    }
  }

  void eval_directional([[resource_table]] ShadowRenderData &srd, uint /*l_idx*/, LightData light)
  {
    light_eval_single(srd, light, true);
  }

  void eval_local([[resource_table]] ShadowRenderData &srd, uint /*l_idx*/, LightData light)
  {
    light_eval_single(srd, light, false);
  }
};

template struct LightEvalCtx<true>;
template struct LightEvalCtx<false>;

template void foreach_visible<LightEvalCtx<true>, ShadowRenderData>(
    const LightRenderData &, float2, float, LightEvalCtx<true> &, ShadowRenderData &);
template void foreach_visible<LightEvalCtx<false>, ShadowRenderData>(
    const LightRenderData &, float2, float, LightEvalCtx<false> &, ShadowRenderData &);

/* NOTE: Doesn't init the closure stack. */
LightEvalCtx<true> init_from_reflect_ctx(LightEvalCtx<false> ctx)
{
  LightEvalCtx<true> ctx_tr;
  ctx_tr.P = ctx.P;
  ctx_tr.Ng = ctx.Ng;
  ctx_tr.V = ctx.V;
  ctx_tr.thickness = ctx.thickness;
  ctx_tr.receiver_light_set = ctx.receiver_light_set;
  ctx_tr.terminator_normal_offset = ctx.terminator_normal_offset;
  ctx_tr.terminator_geometry_offset = ctx.terminator_geometry_offset;
  return ctx_tr;
}

struct LightEvalData {
  [[resource_table]] srt_t<ShadowRenderData> shadow_data;
  [[resource_table]] srt_t<LightRenderData> light_data;

  [[compilation_constant]] int light_closure_eval_count;

  void eval_reflection(LightEvalCtx<false> &ctx, float2 pixel, float vPz)
  {
#ifdef SKIP_LIGHT_EVAL
    return;
#endif
    [[resource_table]] ShadowRenderData &srd = shadow_data;
    [[resource_table]] LightRenderData &lrd = light_data;
    foreach_visible(lrd, pixel, vPz, ctx, srd);
  }

  void eval_transmission(LightEvalCtx<true> &ctx, float2 pixel, float vPz)
  {
#ifdef SKIP_LIGHT_EVAL
    return;
#endif
    [[resource_table]] ShadowRenderData &srd = shadow_data;
    [[resource_table]] LightRenderData &lrd = light_data;
    foreach_visible(lrd, pixel, vPz, ctx, srd);
  }
};

}  // namespace eevee::light
