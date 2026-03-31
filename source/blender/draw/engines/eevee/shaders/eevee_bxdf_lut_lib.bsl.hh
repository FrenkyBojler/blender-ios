/* SPDX-FileCopyrightText: 2017-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_defines.hh"
#include "eevee_utility_tx_lib.glsl"
#include "gpu_shader_compat.hh"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

namespace eevee::lut {

struct BrdfGGX {
  float scale;
  float bias;
  float metal_bias;

  float4 pack() const
  {
    return float4(scale, bias, metal_bias, 0.0f);
  }

  static BrdfGGX unpack(float4 data)
  {
    return {.scale = data.x, .bias = data.y, .metal_bias = data.z};
  }

  static float2 coords_from_params(float cos_theta, float roughness)
  {
    return float2(roughness, sqrt(saturate(1.0f - cos_theta)));
  }

  static BrdfGGX from_utility_tx(const sampler2DArray &util_tx, float cos_theta, float roughness)
  {
    const float2 coords = coords_from_params(cos_theta, roughness);
    const float4 data = utility_tx_sample_lut(util_tx, coords, UTIL_BSDF_LAYER);
    return unpack(data);
  }
};

struct BsdfGGX {
  float scale;
  float bias;
  float transmission_factor;

  float4 pack() const
  {
    return float4(scale, bias, transmission_factor, 0.0f);
  }

  static BsdfGGX unpack(float4 data)
  {
    return {.scale = data.x, .bias = data.y, .transmission_factor = data.z};
  }

  static float3 coords_from_params(float cos_theta, float roughness, float ior)
  {
    /* IOR is the sine of the critical angle. */
    float critical_cos = sqrt(1.0f - ior * ior);

    float3 coords;
    coords.x = square(ior);
    coords.y = cos_theta;
    coords.y -= critical_cos;
    coords.y /= (coords.y > 0.0f) ? (1.0f - critical_cos) : critical_cos;
    coords.y = coords.y * 0.5f + 0.5f;
    coords.z = roughness;

    return saturate(coords);
  }

  static BsdfGGX from_utility_tx(const sampler2DArray &util_tx,
                                 float cos_theta,
                                 float roughness,
                                 float ior)
  {
    const float3 coords = coords_from_params(cos_theta, roughness, ior);
    const float4 data = utility_tx_sample_bsdf_lut(util_tx, coords.xy, coords.z);
    return unpack(data);
  }
};

struct BtdfGGXGt1 {
  float transmission_factor;

  float4 pack() const
  {
    return float4(transmission_factor, 0.0f, 0.0f, 0.0f);
  }

  static BtdfGGXGt1 unpack(float4 data)
  {
    return {.transmission_factor = data.w};
  }

  static float3 coords_from_params(float cos_theta, float roughness, float f0)
  {
    return float3(sqrt(f0), sqrt(1.0f - cos_theta), roughness);
  }

  static BtdfGGXGt1 from_utility_tx(const sampler2DArray &util_tx,
                                    float cos_theta,
                                    float roughness,
                                    float f0)
  {
    const float3 coords = coords_from_params(cos_theta, roughness, f0);
    const float4 data = utility_tx_sample_bsdf_lut(util_tx, coords.xy, coords.z);
    return unpack(data);
  }
};

}  // namespace eevee::lut
