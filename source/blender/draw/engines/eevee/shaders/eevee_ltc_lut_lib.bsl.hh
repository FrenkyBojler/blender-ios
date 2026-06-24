/* SPDX-FileCopyrightText: 2017-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_bxdf_types.bsl.hh"
#include "eevee_defines.hh"
#include "eevee_octahedron_lib.bsl.hh"
#include "eevee_utility_tx.bsl.hh"
#include "gpu_shader_compat.hh"
#include "gpu_shader_utildefines_lib.glsl"

namespace eevee::lut {

namespace detail {

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

namespace detail {

float3x3 unpack_matrix_isotropic(float4 v)
{
  return float3x3(float3(v.x, 0, v.y), float3(0, 1, 0), float3(v.z, 0, v.w));
}

float4 pack_matrix_isotropic(float3x3 M)
{
  return float4(M[0][0], M[0][2], M[2][0], M[2][2]);
}

float2 lut_coords_get(float cos_theta, float roughness)
{
  return float2(roughness, sqrt(saturate(1.0f - cos_theta)));
}

/**
 * Compute the 3rd column of the matrix inverse.
 */
float3 inverse_z(float3x3 M)
{
  /* NOTE(not_mark): this can be simplified for the isotropic case as half the entries are 0. */
  float3 adjoint_x = float3(+(M[1][1] * M[2][2] - M[2][1] * M[1][2]),
                            -(M[0][1] * M[2][2] - M[2][1] * M[0][2]),
                            +(M[0][1] * M[1][2] - M[1][1] * M[0][2]));
  float3 adjoint_z = float3(+(M[1][0] * M[2][1] - M[2][0] * M[1][1]),
                            -(M[0][0] * M[2][1] - M[2][0] * M[0][1]),
                            +(M[0][0] * M[1][1] - M[1][0] * M[0][1]));

  float det = dot(adjoint_x, float3(M[0][0], M[1][0], M[2][0]));
  float3 z = adjoint_z / det;

  return z;
}

}  // namespace detail

/**
 * Pack LTC matrix inverse and associated data to uint4.
 */
packed_uint4 pack(LtcData ltc_data)
{
  float4 v = detail::pack_matrix_isotropic(ltc_data.Minv);
  float2 octahedral_D = octahedral_uv_from_direction(ltc_data.D);

  packed_uint4 ltc_data_packed;

  ltc_data_packed.x = packHalf2x16(v.xy);
  ltc_data_packed.y = packHalf2x16(v.zw);
  ltc_data_packed.z = packHalf2x16(octahedral_D);
  /* NOTE: attenuation_factor can be packed, leaving ~24b spare room. */
  ltc_data_packed.w = floatBitsToUint(ltc_data.attenuation_factor);

  return ltc_data_packed;
}

/**
 * Unpack LTC matrix inverse and associated data from uint4.
 */
LtcData unpack(packed_uint4 ltc_data_packed)
{
  float4 v = float4(unpackHalf2x16(ltc_data_packed.x), unpackHalf2x16(ltc_data_packed.y));
  float2 octahedral_D = unpackHalf2x16(ltc_data_packed.z);

  LtcData ltc_data;

  ltc_data.Minv = detail::unpack_matrix_isotropic(v);
  ltc_data.D = octahedral_uv_to_direction(octahedral_D);
  ltc_data.attenuation_factor = uintBitsToFloat(ltc_data_packed.w);

  return ltc_data;
}

/**
 * Sample a packed ltc matrix from the LUT
 */
packed_uint4 sample_utility_tx([[resource_table]] const UtilityTexture &util_tx,
                               float cos_theta,
                               float roughness)
{
  const float2 coords = detail::lut_coords_get(cos_theta, roughness);
  float4 v = util_tx.sample_lut(coords, UTIL_LTC_MAT_LAYER);

  LtcData ltc_data;
  ltc_data.Minv = detail::unpack_matrix_isotropic(v);
  ltc_data.D = detail::inverse_z(ltc_data.Minv);
  ltc_data.attenuation_factor = 1.0f - saturate(3.0f * roughness);
  return pack(ltc_data);
}

/**
 * Return a packed identity matrix, producing a plain cosine distribution.
 */
packed_float4 identity()
{
  float4 v = float4(1.0f, 0.0f, 0.0f, 1.0f);
  return v;
}
}  // namespace eevee::lut::ltc
