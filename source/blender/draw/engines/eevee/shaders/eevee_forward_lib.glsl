/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/**
 * Forward lighting evaluation: Lighting is evaluated during the geometry rasterization.
 *
 * This is used by alpha blended materials and materials using Shader to RGB nodes.
 */

#include "draw_model_lib.glsl"
#include "eevee_colorspace_lib.bsl.hh"
#include "eevee_light_eval.bsl.hh"
#include "eevee_lightprobe_eval_lib.glsl"
#include "eevee_nodetree_closures_lib.glsl"
#include "eevee_subsurface_lib.glsl"
#include "gpu_shader_codegen_lib.glsl"

/* Allow static compilation of forward materials. */
#ifndef CLOSURE_BIN_COUNT
#  define CLOSURE_BIN_COUNT SRT_CONSTANT_light_closure_eval_count
#endif

#ifndef GLSL_CPP_STUBS
#  if CLOSURE_BIN_COUNT != SRT_CONSTANT_light_closure_eval_count
#    error Closure data count and eval count must match
#  endif
#endif

void forward_lighting_eval(Thickness thickness, float3 &radiance, float3 &transmittance)
{
  float vPz = dot(drw_view_forward(), g_data.P) - dot(drw_view_forward(), drw_view_position());
  float3 V = drw_world_incident_vector(g_data.P);

  eevee::light::LightEvalCtx<false> ctx;
  for (int i = 0; i < SRT_CONSTANT_light_closure_eval_count; i++) {
    ClosureUndetermined cl = g_closure_get(uchar(i));
    eevee::light::closure_set(ctx.stack, uchar(i), closure_light_new(cl, V));
  }

  ctx.P = g_data.P;
  ctx.Ng = g_data.Ng;
  ctx.V = V;
  ctx.thickness = thickness;

  /* TODO(fclem): If transmission (no SSS) is present, we could reduce light_closure_eval_count
   * by 1 for this evaluation and skip evaluating the transmission closure twice. */
  ObjectInfos object_infos = drw_infos[drw_resource_id()];
  ctx.receiver_light_set = receiver_light_set_get(object_infos);
  ctx.terminator_normal_offset = object_infos.shadow_terminator_normal_offset;
  ctx.terminator_geometry_offset = object_infos.shadow_terminator_geometry_offset;

  [[resource_table]] eevee::light::LightEvalData &lrd = resource_table_get(
      eevee::light::LightEvalData);
  lrd.eval_reflection(ctx, gl_FragCoord.xy, vPz);

#if defined(MAT_SUBSURFACE) || defined(MAT_REFRACTION) || defined(MAT_TRANSLUCENT)
  ClosureUndetermined cl_transmit = g_closure_get(0);
  if (cl_transmit.type == CLOSURE_BSDF_TRANSLUCENT_ID ||
      cl_transmit.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID ||
      cl_transmit.type == CLOSURE_BSSRDF_BURLEY_ID)
  {
    eevee::light::LightEvalCtx<true> ctx_tr = eevee::light::init_from_reflect_ctx(ctx);
    ctx_tr.stack.cl[0] = closure_light_new(cl_transmit, V, thickness);

    /* NOTE: Only evaluates `stack.cl[0]`. */
    lrd.eval_transmission(ctx_tr, gl_FragCoord.xy, vPz);

    if (cl_transmit.type == CLOSURE_BSSRDF_BURLEY_ID) {
#  if defined(MAT_SUBSURFACE)
      /* Apply transmission profile onto transmitted light and sum with reflected light. */
      float3 sss_profile = subsurface_transmission(to_closure_subsurface(cl_transmit).sss_radius,
                                                   thickness.value());
      ctx.stack.cl[0].light_shadowed += ctx_tr.stack.cl[0].light_shadowed * sss_profile;
      ctx.stack.cl[0].light_unshadowed += ctx_tr.stack.cl[0].light_unshadowed * sss_profile;
#  endif
    }
    else {
      ctx.stack.cl[0].light_shadowed = ctx_tr.stack.cl[0].light_shadowed;
      ctx.stack.cl[0].light_unshadowed = ctx_tr.stack.cl[0].light_unshadowed;
    }
  }
#endif

  LightProbeSample samp = lightprobe_load(gl_FragCoord.xy, g_data.P, g_data.Ng, V);

  float clamp_indirect_sh = uniform_buf.clamp.surface_indirect;
  samp.volume_irradiance = spherical_harmonics::clamp_energy(samp.volume_irradiance,
                                                             clamp_indirect_sh);

  /* Combine all radiance. */
  float3 radiance_direct = float3(0.0f);
  float3 radiance_indirect = float3(0.0f);
  for (uchar i = 0; i < SRT_CONSTANT_light_closure_eval_count; i++) {
    ClosureUndetermined cl = g_closure_get_resolved(i, 1.0f);
    if (cl.weight > CLOSURE_WEIGHT_CUTOFF) {
      float3 direct_light = eevee::light::closure_get(ctx.stack, i).light_shadowed;
      float3 indirect_light = lightprobe_eval(samp, cl, g_data.P, V, thickness);

      if ((cl.type == CLOSURE_BSDF_TRANSLUCENT_ID ||
           cl.type == CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID) &&
          (thickness.value() != 0.0f))
      {
        /* We model two transmission event, so the surface color need to be applied twice. */
        cl.color *= cl.color;
      }

      radiance_direct += direct_light * cl.color;
      radiance_indirect += indirect_light * cl.color;
    }
  }
  /* Light clamping. */
  float clamp_direct = uniform_buf.clamp.surface_direct;
  float clamp_indirect = uniform_buf.clamp.surface_indirect;

  radiance_direct = colorspace::brightness_clamp_max(radiance_direct, clamp_direct);
  radiance_indirect = colorspace::brightness_clamp_max(radiance_indirect, clamp_indirect);

  radiance_direct *= uniform_buf.clamp.direct_scale;
  radiance_indirect *= uniform_buf.clamp.indirect_scale;

  radiance = radiance_direct + radiance_indirect + g_emission;

  transmittance = g_transmittance;
}
