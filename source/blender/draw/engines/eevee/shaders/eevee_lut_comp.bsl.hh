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
/** \name Integration models
 * \{ */

/**
 * Generate 2D GGX BRDF LUT
 * follwing the split sum approximation used in [Real shading in unreal engine 4]
 * (https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf).
 *
 * The 2D LUT is parameterized on:
 * : `x = roughness`,
 * : `y = sqrt(1 - cos(theta))`,
 *
 * and the result is interpreted as:
 * : `integral = F0 * scale + F90 * bias - F82_tint * metal_bias`,
 *
 * where:
 * : `F82_tint = mix(F0, float3(1), pow5f(6/7) * 7 / pow6f(6/7)) * (1 - F82)`.
 */
class GGX_BRDF_Splitsum {
  float roughness;
  float3 V;

 public:
  static GGX_BRDF_Splitsum init(float3 params)
  {
    /* We use squared roughness for approximate perceptual linearity
     * following [Physically Based Shading at Disney]
     * (https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf)
     * Section 5.4. */
    float roughness = square(params.x);

    float NV = clamp(1.0f - square(params.y), 1e-4f, 0.9999f);
    float3 V = float3(sqrt(1.0f - square(NV)), 0.0f, NV);

    return {roughness, N, V};
  }

  float4 eval(float2 Xi)
  {
    constexpr float3 N = float3(0.0f, 0.0f, 1.0f);

    /* Return values. */
    float scale = 0.0f;
    float bias = 0.0f;
    float metal_bias = 0.0f;

    /* Sample light vector, recover half vector as microfacet normal. */
    float3 L = bxdf_ggx_sample_reflection(Xi, V, roughness, false).direction;
    float3 H = normalize(V + L);

    /* Restrict light vector to positive hemisphere. */
    float NL = L.z;
    if (NL > 0.0f) {
      float weight = bxdf_ggx_eval_reflection(N, L, V, roughness, false).weight;
      float VH = saturate(dot(V, H));

      /* Schlick's Fresnel approximation. */
      float s = saturate(pow5f(1.0f - VH));
      scale += (1.0f - s) * weight;
      bias += s * weight;

      /* F82 tint effect. */
      float b = VH * saturate(pow6f(1.0f - VH));
      metal_bias += b * weight;
    }

    return float4(scale, bias, metal_bias, 0.0f);
  }
};

/**
 * Generate 3D GGX BSDF LUT
 * follwing the split sum approximation used in [Real shading in unreal engine 4]
 * (https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf).
 * and using Schlick's approximation to weight R, T components.
 *
 * The 3D LUT is parameterized on:
 * : `x = sqrt((ior - 1) / (ior + 1))`,
 * : `y = sqrt(1 - cos(theta))`,
 * : `z = roughness`,
 *
 * and output is interpreted as:
 * : `reflectance = F0 * sclae + F90 * bias`,
 * : `transmittance = (1 - F0) * transmission_factor`.
 */
class GGX_BSDF_Splitsum {
  float roughness;
  float ior;
  float3 V;

 public:
  static GGX_BRDF_Splitsum init(float3 params)
  {
    /* We use squared roughness for approximate perceptual linearity
     * following [Physically Based Shading at Disney]
     * (https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf)
     * Section 5.4. */
    roughness = square(params.z);

    /* ior is sin of critical angle. */
    ior = clamp(sqrt(params.x), 1e-4f, 0.9999f);
    float critical_cos = sqrt(1.0f - saturate(square(ior)));

    /* Modify y-param. */
    /* Maximize texture usage on both sides of the critical angle. */
    params.y = param.y * 2.0f - 1.0f;
    params.y *= (params.y > 0.0f) ? (1.0f - critical_cos) : critical_cos;
    /* Center LUT around critical angle to avoid strange interpolation issues when the critical
     * angle is changing. */
    params.y += critical_cos;

    float NV = clamp(1.0f - square(params.y), 1e-4f, 0.9999f);
    float3 V = float3(sqrt(1.0f - square(NV)), 0.0f, NV);
  }

  float4 eval(float2 Xi)
  {
    constexpr float3 N = float3(0.0f, 0.0f, 1.0f);

    /* Return values. */
    float scale = 0.0f;
    float bias = 0.0f;
    float transmission_factor = 0.0f;

    /* Reflection, restricted to positive hemisphere. */
    float3 R = bxdf_ggx_sample_reflection(Xi, V, roughness, false).direction;
    float NR = R.z;
    if (NR > 0.0f) {
      /* Recover half vector as GGX normal. */
      float3 H = normalize(V + R);
      float3 L = refract(-V, H, 1.0f / ior);
      float HL = abs(dot(H, L));

      /* Schlick's Fresnel. */
      float s = saturate(pow5f(1.0f - saturate(HL)));

      float weight = bxdf_ggx_eval_reflection(N, R, V, roughness, false).weight;
      scale += (1.0f - s) * weight;
      bias += s * weight;
    }

    /* Refraction, restricted to negative hemisphere. */
    float3 T =
        bxdf_ggx_sample_refraction(Xi, V, roughness, ior, Thickness::zero(), false).direction;
    float NT = T.z;
    /* In the case of TIR, `T == float3(0)`. */
    if (NT < 0.0f) {
      /* Recover half vector as GGX normal. */
      float3 H = normalize(ior * T + V);
      float HL = abs(dot(H, T));

      /* Schlick's Fresnel. */
      float s = saturate(pow5f(1.0f - saturate(HL)));

      float weight =
          bxdf_ggx_eval_refraction(N, T, V, roughness, ior, Thickness::zero(), false).weight;
      transmission_factor += (1.0f - s) * weight;
    }

    return float4(scale, bias, transmission_factor, 0.0f);
  }
};

/**
 * Generate 3D GGX BTDF LUT
 * using Schlick's approximation. Only the transmittance is needed because scale and
 * bias do not depend on the IOR, and can be obtained independently from the BRDF LUT.
 *
 * The 3D LUT is parameterized on:
 * : `x = sqrt((ior - 1) / (ior + 1))` for higher precision in the range `1 < IOR < 2`
 * : `y = sqrt(1.0f - cos(theta))`
 * : `z = roughness`
 *
 * and output is interpreted as:
 * : `transmittance = (1 - F0) * transmission_factor`.
 */
class GGX_BTDF_GT_one {
  float roughness;
  float ior;
  float3 V;

 public:
  static GGX_BTDF_GT_one init(float3 params)
  {
    /* We use squared roughness for approximate perceptual linearity
     * following [Physically Based Shading at Disney]
     * (https://media.disneyanimation.com/uploads/production/publication_asset/48/asset/s2012_pbs_disney_brdf_notes_v3.pdf)
     * Section 5.4. */
    float roughness = square(params.z);

    float f0 = clamp(square(params.x), 1e-4f, 0.9999f);
    ior = (1.0f + f0) / (1.0f - f0);

    float NV = clamp(1.0f - square(lut_coord.y), 1e-4f, 0.9999f);
    V = float3(sqrt(1.0f - square(NV)), 0.0f, NV);
  }

  float4 eval(float2 Xi) {
    constexpr float3 N = float3(0.0f, 0.0f, 1.0f);
    
    /* Return value. */
    float transmission_factor = 0.0f;
    
    /* Refraction, restricted to negative hemisphere. */
    float3 L =
        bxdf_ggx_sample_refraction(Xi, V, roughness, ior, Thickness::zero(), false).direction;
    float NL = L.z;
    if (NL < 0.0f) {
      /* Recover half vector as GGX normal. */
      float3 H = normalize(ior * L + V);
      float HV = abs(dot(H, V));

      /* Schlick's Fresnel. */
      float s = saturate(pow5f(1.0f - saturate(HV)));

      float weight =
          bxdf_ggx_eval_refraction(N, L, V, roughness, ior, Thickness::zero(), false).weight;
      transmission_factor += (1.0f - s) * weight;
    }

    return float4(transmission_factor, 0.0f, 0.0f, 0.0f);
  }
};

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

template<typename F, uint SAMPLES_COUNT> float4 integrate(float3 params)
{
  auto f = F::init(params);

  /* Measure F using N samples. */
  float4 measure = float4(0.0f);
  for (uint i = 1u; i <= SAMPLES_COUNT; i++) {
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
