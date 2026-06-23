/* SPDX-FileCopyrightText: 2017-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_bxdf_types.bsl.hh"
#include "eevee_defines.hh"
#include "eevee_utility_tx.bsl.hh"
#include "gpu_shader_compat.hh"
#include "gpu_shader_utildefines_lib.glsl"

namespace eevee::lut {

namespace detail {

/**
 * Compute the 3rd column of the matrix inverse.
 */
float3 inverse_z(float3x3 M)
{
  float3 adjoint_x = float3(+(M[1][1] * M[2][2] - M[2][1] * M[1][2]),
                            -(M[0][1] * M[2][2] - M[2][1] * M[0][2]),
                            +(M[0][1] * M[1][2] - M[1][1] * M[0][2]));
  float3 adjoint_z = float3(+(M[1][0] * M[2][1] - M[2][0] * M[1][1]),
                            -(M[0][0] * M[2][1] - M[2][0] * M[0][1]),
                            +(M[0][0] * M[1][1] - M[1][0] * M[0][1]));
  float det = dot(adjoint_x, float3(M[0][0], M[1][0], M[2][0]));
  return adjoint_z / det;
}

float2 lut_coords_get(float cos_theta, float roughness)
{
  return float2(roughness, sqrt(saturate(1.0f - cos_theta)));
}
}  // namespace detail

struct LTCMatrixData {
  uint2 Minv_iso_packed;
  uint2 clamp_params_packed;

  static LTCMatrixData from(packed_uint4 data)
  {
    return {
        .Minv_iso_packed = data.xy,
        .clamp_params_packed = data.zw,
    };
  }

  float3x3 unpack_Minv() const
  {
    float2 vx = unpackHalf2x16(Minv_iso_packed.x);
    float2 vy = unpackHalf2x16(Minv_iso_packed.y);
    return float3x3(float3(vx.x, 0, vx.y), float3(0, 1, 0), float3(vy.x, 0, vy.y));
  }

  float4 unpack_clamp_params() const
  {
    return float4(unpackHalf2x16(clamp_params_packed.x), unpackHalf2x16(clamp_params_packed.y));
  }

  static packed_uint4 identity()
  {
    float4 v = float4(1.0f, 0.0f, 0.0f, 1.0f);
    return packed_uint4(packHalf2x16(v.xy), packHalf2x16(v.zw), 0.0f, 0.0f);
  }

  static packed_uint4 sample_utility_tx([[resource_table]] const UtilityTexture &util_tx,
                                        float cos_theta,
                                        float roughness)
  {

    /* Sample utility texture to obtain M or Minv. */
    const float2 coords = detail::lut_coords_get(cos_theta, roughness);
    float4 v = util_tx.sample_lut(coords, UTIL_LTC_MAT_LAYER);

    /* Obtain the z-column of inv(Minv), as dominant BxDF lobe direction. */
    /* TODO: optimize specifically for isotropic case. */
    float3x3 Minv = float3x3(float3(v.x, 0, v.y), float3(0, 1, 0), float3(v.z, 0, v.w));
    float3 D = normalize(inverse(Minv)[2]);  // normalize(detail::inverse_z(Minv));

    /* Store attenuation factor for GGX lobe. */
    float clamp_factor = 1.0f - saturate(3.0f * roughness);

    return packed_uint4(packHalf2x16(float2(Minv[0].xz)),
                        packHalf2x16(float2(Minv[2].xz)),
                        packHalf2x16(D.xy),
                        packHalf2x16(float2(D.z, clamp_factor)));
  }
};

}  // namespace eevee::lut

namespace eevee::lut::ltc {

// struct Data {
//   packed_float2 Minv_half;
//   float Minv_determinant;
//   packed_float3 dominant_direction;

//   static LTCData identity() {}
//   static LTCData unpack(float4 data) {}

//   float3x3 Minv() const {}

//   // float3 M_dominant_direction() const {}
//   // float Minv_determinant() const {}
// };

float2 lut_coords_get(float cos_theta, float roughness)
{
  return float2(roughness, sqrt(saturate(1.0f - cos_theta)));
}

/**
 * Sample a packed ltc matrix from the LUT
 */
packed_float4 sample_utility_tx([[resource_table]] const UtilityTexture &util_tx,
                                float cos_theta,
                                float roughness)
{
  const float2 coords = lut_coords_get(cos_theta, roughness);
  return util_tx.sample_lut(coords, UTIL_LTC_MAT_LAYER);
}

/**
 * Return a packed identity matrix, resulting in a plain cosine distribution.
 */
packed_float4 identity()
{
  return float4(1.0f, 0.0f, 0.0f, 1.0f);
}

/**
 * Load inverse LTC matrix M^{-1} from packed ltc value.
 */
float3x3 unpack(packed_float4 v)
{
  return float3x3(float3(v.x, 0, v.y), float3(0, 1, 0), float3(v.z, 0, v.w));
}
}  // namespace eevee::lut::ltc
