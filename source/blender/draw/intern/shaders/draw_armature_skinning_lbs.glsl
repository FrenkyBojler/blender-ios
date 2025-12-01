/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compute shader for armature modifier deformation using Linear Blend Skinning.
 */

#include "draw_skinning_infos.hh"

COMPUTE_SHADER_CREATE_INFO(draw_armature_skinning_lbs)

float2 unpack_weights_from_uint(uint x)
{
  const float inv65535 = 1.0f / 65535.0f;
  uint w0 = x & 0xFFFFu;
  uint w1 = (x >> 16) & 0xFFFFu;
  return float2(float(w0) * inv65535, float(w1) * inv65535);
}

float4 unpack_weights_from_two_uints(uint a, uint b)
{
  float2 p0 = unpack_weights_from_uint(a);
  float2 p1 = unpack_weights_from_uint(b);
  return float4(p0.x, p0.y, p1.x, p1.y);
}

uvec2 unpack_indices_from_uint(uint x)
{
  uint i0 = x & 0xFFFFu;
  uint i1 = (x >> 16) & 0xFFFFu;
  return uvec2(i0, i1);
}

uvec4 unpack_indices_from_two_uints(uint a, uint b)
{
  uvec2 p0 = unpack_indices_from_uint(a);
  uvec2 p1 = unpack_indices_from_uint(b);
  return uvec4(p0.x, p0.y, p1.x, p1.y);
}

float2 sign_not_zero(float2 v)
{
  return float2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}

float3 unpack_octahedral(float2 p)
{
  float3 v = float3(p.x, p.y, 1.0f - abs(p.x) - abs(p.y));
  if (v.z < 0.0f) {
    v.xy = (1.0f - abs(v.yx)) * sign_not_zero(v.xy);
  }
  return normalize(v);
}

void main()
{
  uint gid = gl_GlobalInvocationID.x;
  if (gid >= uint(vertex_count)) {
    return;
  }

  uint idx_u0 = indices_buf[gid].x;
  uint idx_u1 = indices_buf[gid].y;
  uvec4 bone_idx = unpack_indices_from_two_uints(idx_u0, idx_u1);

  uint w_u0 = weights_buf[gid].x;
  uint w_u1 = weights_buf[gid].y;

  /* Early out for unweighted vertices. */
  if (w_u0 == 0u && w_u1 == 0u) {
    float4 P_rest = pos_buf[gid];
    float2 N_packed = nor_buf[gid];
    float3 N_rest = unpack_octahedral(N_packed);
    float4 T_rest = tan_buf[gid];
    out_skinned_pos[gid] = P_rest;
    out_skinned_nor[gid] = float4(N_rest, 0.0f);
    out_skinned_tan[gid] = T_rest;
    return;
  }

  float4 weights = unpack_weights_from_two_uints(w_u0, w_u1);

  float3 P_rest = float3(pos_buf[gid]);
  float4 P_rest_f = float4(P_rest, 1.0f);
  float2 N_packed = nor_buf[gid];
  float3 N_rest = unpack_octahedral(N_packed);
  float4 T_rest = tan_buf[gid];

  float3 co_accum = float3(0.0f);
  float3 N_accum = float3(0.0f);
  float3 T_accum = float3(0.0f);
  float total_weight = 0.0f;

  for (int k = 0; k < 4; ++k) {
    uint bi = bone_idx[k];
    float w = weights[k];

    if (w > 0.0f && bi != 0xFFFFu) {
      mat4 bm = bonemat_buf[bi];

      float3 P_trans = (bm * P_rest_f).xyz;
      float3 P_eval = P_trans - P_rest;
      co_accum += P_eval * w;

      float3 transformed_nor = mat3(bm) * N_rest;
      float3 N_eval = transformed_nor - N_rest;
      N_accum += N_eval * w;

      float3 transformed_tan = mat3(bm) * T_rest.xyz;
      float3 T_eval = transformed_tan - T_rest.xyz;
      T_accum += T_eval * w;

      total_weight += w;
    }
  }

  float3 P_final = P_rest + co_accum;
  float3 N_final = normalize(N_rest + N_accum);
  float3 T_final = normalize(T_rest.xyz + T_accum);

  out_skinned_pos[gid] = float4(P_final, 1.0f);
  out_skinned_nor[gid] = float4(N_final, 0.0f);
  out_skinned_tan[gid] = float4(T_final, T_rest.w);
}
