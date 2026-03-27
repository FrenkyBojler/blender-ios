/* SPDX-FileCopyrightText: 2017-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Pipelines to generate Look Up Tables for BxDFs. Not used in the default configuration as the
 * tables are stored in the blender executable. This is used for reference or to update them.
 */

#pragma once

#include "eevee_bxdf_sampling_lib.glsl"
#include "eevee_defines.hh"
#include "eevee_light_shared.hh"
#include "eevee_precompute_shared.hh"
#include "eevee_sampling_lib.glsl"
#include "eevee_subsurface_shared.hh"
#include "eevee_uniform_shared.hh"
#include "gpu_shader_compat.hh"
#include "gpu_shader_math_base_lib.glsl"

namespace eevee {

struct LUT {
  [[image(0, read_write, SFLOAT_32_32_32_32)]] image3D image;

  [[push_constant]] const int type;
  [[push_constant]] const int3 extent;
};

/* -------------------------------------------------------------------- */
/** \name Integration
 * \{ */

struct GGX_BRDF_Splitsum {
  float roughness;
  float3 V;

  static GGX_BRDF_Splitsum init(float3 params)
  {
    /* Squared roughness for approximate perceptual linearity.
     * see [Physically Based Shading at Disney]
     * (https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf)
     * Section 5.4. */
    float roughness = square(params.x);

    float NV = clamp(1.0f - square(params.y), 1e-4f, 0.9999f);
    float3 V = float3(sqrt(1.0f - square(NV)), 0.0f, NV);
    float3 N = float3(0.0f, 0.0f, 1.0f);

    return {roughness, N, V};
  }

  static float4 pack(float scale, float bias, float transmission_factor)
  {
    return float4(scale, bias, transmission_factor, 0.0f);
  }

  float4 eval(float2 Xi)
  {
    float scale = 0.0f;
    float bias = 0.0f;
    float metal_bias = 0.0f;
    
    constexpr float3 N = float3(0.0f, 0.0f, 1.0f);

    /* Microfacet normal. */
    /* Light vector, half vector, cosine to microfacet normal. */
    float3 L = bxdf_ggx_sample_reflection(Xi, V, roughness, false).direction;
    float3 H = normalize(V + L);
    float NL = L.z;

    if (NL <= 0.0f) {
      return float4(0);
    }

    float weight = bxdf_ggx_eval_reflection(N, L, V, roughness, false).weight;
    float VH = saturate(dot(V, H));

    /* Schlick's Fresnel approximation. */
    float s = saturate(pow5f(1.0f - VH));
    scale += (1.0f - s) * weight;
    bias += s * weight;

    /* F82 tint effect. */
    float b = VH * saturate(pow6f(1.0f - VH));
    metal_bias += b * weight;

    return pack(scale, bias, transmission_factor);
  }
};

float4 ggx_brdf_split_sum(float3 lut_coord)
{
  // return float4(scale, bias, metal_bias, 0.0f);
  return float4(0);
}

float4 ggx_bsdf_split_sum(float3 lut_coord)
{
  // return float4(scale, bias, transmission_factor, 0.0f);
  return float4(0);
}

float4 ggx_btdf_gt_one(float3 lut_coord)
{
  // return float4(transmission_factor, 0.0f, 0.0f, 0.0f);
  return float4(0);
}

float4 burley_sss_translucency(float3 lut_coord)
{
  /* profile is float3 */
  // return float4(profile, 0.0f);
  return float4(0);
}

float4 random_walk_sss_translucency(float3 lut_coord)
{
  // return float4(profile, 0.0f);
  return float4(0);
}

template<typename F, uint N> float4 integrate(float3 params)
{
  auto f = F::init(params);

  /* Measure F using N samples. */
  float4 measure = float4(0.0f);
  for (uint i = 0u; i < N; i++) {
    /* Warp sequence to a random point on the unit cylinder. */
    float2 rand = hammersley_2d(i, N);
    float2 Xi = sample_cylinder(rand);

    /* Take sample and add to incremental measure. */
    measure += (f.eval(Xi) - measure) / float(i);
  }

  return measure;
}

/** \} */

[[compute]] [[local_size(LUT_WORKGROUP_SIZE, LUT_WORKGROUP_SIZE)]]
void comp_main([[global_invocation_id]] const uint3 global_id, [[resource_table]] LUT &lut)
{
  float4 v = integrate<GGX_BRDF_Splitsum, 256>();
}

PipelineCompute lut_comp_pass(comp_main);

}  // namespace eevee
