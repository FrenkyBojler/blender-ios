/* SPDX-FileCopyrightText: 2017-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GPU_shader_shared_utils.hh"
#include "eevee_bxdf_types.bsl.hh"
#include "eevee_defines.hh"
#include "eevee_octahedron_lib.bsl.hh"
#include "eevee_utility_tx.bsl.hh"
#include "gpu_shader_compat.hh"
#include "gpu_shader_math_matrix_construct_lib.glsl"
#include "gpu_shader_math_vector_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

// namespace eevee::lut {

// namespace detail {

// float2 lut_coords_get(float cos_theta, float roughness)
// {
//   return float2(roughness, sqrt(saturate(1.0f - cos_theta)));
// }
// }  // namespace detail

// struct LTCMatrixData {
//   uint2 Minv_iso_packed;
//   uint2 clamp_params_packed;

//   static LTCMatrixData from(packed_uint4 data)
//   {
//     return {
//         .Minv_iso_packed = data.xy,
//         .clamp_params_packed = data.zw,
//     };
//   }

//   float3x3 unpack_Minv() const
//   {
//     float2 vx = unpackHalf2x16(Minv_iso_packed.x);
//     float2 vy = unpackHalf2x16(Minv_iso_packed.y);
//     return float3x3(float3(vx.x, 0, vx.y), float3(0, 1, 0), float3(vy.x, 0, vy.y));
//   }

//   float4 unpack_clamp_params() const
//   {
//     return float4(unpackHalf2x16(clamp_params_packed.x), unpackHalf2x16(clamp_params_packed.y));
//   }

//   static packed_uint4 identity()
//   {
//     float4 v = float4(1.0f, 0.0f, 0.0f, 1.0f);
//     return packed_uint4(packHalf2x16(v.xy), packHalf2x16(v.zw), 0.0f, 0.0f);
//   }

//   static packed_uint4 sample_utility_tx([[resource_table]] const UtilityTexture &util_tx,
//                                         float cos_theta,
//                                         float roughness)
//   {
//     /* Sample utility texture to obtain M or Minv. */
//     const float2 coords = detail::lut_coords_get(cos_theta, roughness);
//     float4 v = util_tx.sample_lut(coords, UTIL_LTC_MAT_LAYER);

//     /* Obtain the z-column of inv(Minv), as dominant BxDF lobe direction. */
//     /* TODO: optimize specifically for isotropic case. */
//     float3x3 Minv = float3x3(float3(v.x, 0, v.y), float3(0, 1, 0), float3(v.z, 0, v.w));
//     float3 D = normalize(inverse(Minv)[2]);  // normalize(detail::inverse_z(Minv));

//     /* Store attenuation factor for GGX lobe. */
//     float clamp_factor = 1.0f - saturate(3.0f * roughness);

//     return packed_uint4(packHalf2x16(float2(Minv[0].xz)),
//                         packHalf2x16(float2(Minv[2].xz)),
//                         packHalf2x16(D.xy),
//                         packHalf2x16(float2(D.z, clamp_factor)));
//   }
// };

// }  // namespace eevee::lut

namespace eevee::lut::ltc {

namespace detail {

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

/**
 * Compute a tangent basis around N, using an incident vector I.
 */
float3x3 tangent_basis(float3 N, float3 I)
{
  float NI = dot(N, I);
  if (NI > 0.999999f) {
    /* Mostly for orthographic view and surfel light eval. */
    return from_up_axis(N);
  }
  /* Construct orthonormal basis around N. */
  float3 T1 = normalize(I - N * NI);
  float3 T2 = cross(N, T1);

  return float3x3(T1, T2, N);
}

float3x3 unpack_matrix_isotropic(float4 v)
{
  return float3x3(float3(v.x, 0, v.y), float3(0, 1, 0), float3(v.z, 0, v.w));
}

float4 pack_matrix_isotropic(float3x3 M)
{
  return float4(M[0][0], M[0][2], M[2][0], M[2][2]);
}

}  // namespace detail

/**
 * Pack LTC matrix inverse and associated data to uint4.
 *
 * The LTC matrix has 9 components. While only four are unknowns in the isotropic case,
 * we pack all nine components for two reasons:
 * 1. We can rotate the LTC matrix into a shading basis beforehand.
 * 2. We leave room for anisotropy support.
 *
 * We use the following packing scheme for the matrix:
 * 1. The index of the largest component is determined as 'hero'.
 * 2. Other components are divided by the hero, so they are bounded to [0, 1].
 * 3. Other components are packed as 8xu8, while the hero is packed as f16.
 * 4. Remaining data is packed in the
 * Matrix components are not normalized, and thus not bounded to [0, 1]. We store them
 * at half precision.
 */
packed_uint4 pack_matrix(LtcData ltc_data)
{
  packed_uint4 ltc_matrix_packed;
  ltc_matrix_packed.x = packHalf2x16(ltc_data.Minv[0].xy);
  ltc_matrix_packed.y = packHalf2x16(float2(ltc_data.Minv[0].z, ltc_data.Minv[1].x));
  ltc_matrix_packed.z = packHalf2x16(float2(ltc_data.Minv[1].z, ltc_data.Minv[2].x));
  ltc_matrix_packed.w = packHalf2x16(ltc_data.Minv[2].yz);
  return ltc_matrix_packed;
}

uint pack_data(LtcData ltc_data)
{
  float2 octahedral_D = octahedral_uv_from_direction(ltc_data.D);
  return packHalf2x16(octahedral_D);
  /* Pack as 11, 11, 10 instead. */
  // return packSnorm4x8(float4(octahedral_D, ltc_data.attenuation_factor, 0));
}

void pack(LtcData ltc_data, packed_uint4 &ltc_matrix_packed, uint &ltc_data_packed)
{
  ltc_matrix_packed = pack_matrix(ltc_data);
  ltc_data_packed = pack_data(ltc_data);
}

/**
 * Unpack LTC matrix inverse and associated data from uint4.
 */
LtcData unpack(packed_uint4 ltc_matrix_packed, uint ltc_data_packed)
{
  LtcData ltc_data;

  float2 v1 = unpackHalf2x16(ltc_matrix_packed.y);
  float2 v2 = unpackHalf2x16(ltc_matrix_packed.z);

  ltc_data.Minv[0].xy = unpackHalf2x16(ltc_matrix_packed.x);
  ltc_data.Minv[0].z = v1.x;
  ltc_data.Minv[1].x = v1.y;
  ltc_data.Minv[1].y = 1.0f;
  ltc_data.Minv[1].z = v2.x;
  ltc_data.Minv[2].x = v2.y;
  ltc_data.Minv[2].yz = unpackHalf2x16(ltc_matrix_packed.w);

  ltc_data.D = octahedral_uv_to_direction(unpackHalf2x16(ltc_data_packed));
  ltc_data.attenuation_factor = 0.0;

  // float4 v3 = unpackSnorm4x8(ltc_data_packed);
  // ltc_data.D = octahedral_uv_to_direction(v3.xy);
  // ltc_data.attenuation_factor = v3.z;

  return ltc_data;
}

/**
 * Sample a packed ltc matrix from the LUT.
 */
LtcData sample_utility_tx([[resource_table]] const UtilityTexture &util_tx,
                          float3 N,
                          float3 I,
                          float roughness)
{
  /* Sample LTC table. */
  const float2 coords = detail::lut_coords_get(dot(N, I), roughness);
  float4 lut_pack = util_tx.sample_lut(coords, UTIL_LTC_MAT_LAYER);

  /* Inverse LTC matrix. */
  float3x3 Minv = detail::unpack_matrix_isotropic(lut_pack);

  /* Construct orthonormal basis around N.  */
  float3x3 T = detail::tangent_basis(N, I);

  /* Rotate area light into basis. */
  Minv = Minv * transpose(T);

  /* Normalize by central value after rotation. */
  float rcp = 1.0f / Minv[1][1];
  Minv[0] *= rcp;
  Minv[1] *= rcp;
  Minv[2] *= rcp;

  LtcData ltc_data;
  ltc_data.Minv = Minv;
  ltc_data.D = normalize(detail::inverse_z(ltc_data.Minv));
  ltc_data.attenuation_factor = saturate(3.0f * roughness);
  return ltc_data;
}

/**
 * Return a packed ltc matrix producing a cosine distribution.
 */
LtcData identity(float3 N, float3 I)
{
  /* Inverse LTC matrix. */
  float3x3 Minv = mat3x3_identity();

  /* Construct orthonormal basis around N.  */
  float3x3 T = detail::tangent_basis(N, I);

  /* Rotate area light into basis. */
  Minv = Minv * transpose(T);

  /* Re-normalize by central value after rotation. */
  float rcp = 1.0f / Minv[1][1];
  Minv[0] *= rcp;
  Minv[1] *= rcp;
  Minv[2] *= rcp;

  LtcData ltc_data;
  ltc_data.Minv = Minv;
  ltc_data.D = normalize(detail::inverse_z(ltc_data.Minv));
  ltc_data.attenuation_factor = 0.0;
  return ltc_data;
}
}  // namespace eevee::lut::ltc
