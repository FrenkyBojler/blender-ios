/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compute shader for armature modifier deformation using Dual Quaternion Skinning.
 */

#include "draw_math_quat_lib.glsl"
#include "draw_skinning_infos.hh"


COMPUTE_SHADER_CREATE_INFO(draw_armature_skinning_dqs)

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
  uint w_u0 = weights_buf[gid].x;
  uint w_u1 = weights_buf[gid].y;
  float4 P_rest_full = pos_buf[gid];
  float2 N_packed = nor_buf[gid];
  float4 T_rest = tan_buf[gid];

  /* Early exit for unweighted vertices */
  if (w_u0 == 0u && w_u1 == 0u) {
    float3 N_rest = unpack_octahedral(N_packed);
    out_skinned_pos[gid] = P_rest_full;
    out_skinned_nor[gid] = float4(N_rest, 0.0f);
    out_skinned_tan[gid] = T_rest;
    return;
  }

  uvec4 bone_idx = unpack_indices_from_two_uints(idx_u0, idx_u1);
  float4 weights = unpack_weights_from_two_uints(w_u0, w_u1);
  float3 P_rest = P_rest_full.xyz;
  float3 N_rest = unpack_octahedral(N_packed);

  GPUDualQuat gpu_dq0, gpu_dq1, gpu_dq2, gpu_dq3;
  if (bone_idx[0] != 0xFFFFu)
    gpu_dq0 = bonedq_buf[bone_idx[0]];
  if (bone_idx[1] != 0xFFFFu)
    gpu_dq1 = bonedq_buf[bone_idx[1]];
  if (bone_idx[2] != 0xFFFFu)
    gpu_dq2 = bonedq_buf[bone_idx[2]];
  if (bone_idx[3] != 0xFFFFu)
    gpu_dq3 = bonedq_buf[bone_idx[3]];

  DualQuat dq_sum;
  dq_sum.quat = float4(0.0, 0.0, 0.0, 0.0);
  dq_sum.trans = float4(0.0, 0.0, 0.0, 0.0);
  dq_sum.scale = float4x4(0.0);
  dq_sum.scale_weight = 0.0;
  dq_sum.quat_weight = 0.0;

  float total_weight = 0.0;

  /* Unrolled loop for better instruction scheduling */

  /* Bone 0 */
  if (weights[0] > 0.0 && bone_idx[0] != 0xFFFFu) {
    DualQuat bone_dq;
    bone_dq.quat = float4(gpu_dq0.quat[1], gpu_dq0.quat[2], gpu_dq0.quat[3], gpu_dq0.quat[0]);
    bone_dq.trans = float4(gpu_dq0.trans[1], gpu_dq0.trans[2], gpu_dq0.trans[3], gpu_dq0.trans[0]);
    bone_dq.scale = float4x4(
        float4(gpu_dq0.scale[0][0], gpu_dq0.scale[0][1], gpu_dq0.scale[0][2], gpu_dq0.scale[0][3]),
        float4(gpu_dq0.scale[1][0], gpu_dq0.scale[1][1], gpu_dq0.scale[1][2], gpu_dq0.scale[1][3]),
        float4(gpu_dq0.scale[2][0], gpu_dq0.scale[2][1], gpu_dq0.scale[2][2], gpu_dq0.scale[2][3]),
        float4(
            gpu_dq0.scale[3][0], gpu_dq0.scale[3][1], gpu_dq0.scale[3][2], gpu_dq0.scale[3][3]));
    bone_dq.scale_weight = gpu_dq0.scale_weight;
    bone_dq.quat_weight = 1.0;
    accumulate_dual_quat_pivot(dq_sum, bone_dq, P_rest, weights[0]);
    total_weight += weights[0];
  }

  /* Bone 1 */
  if (weights[1] > 0.0 && bone_idx[1] != 0xFFFFu) {
    DualQuat bone_dq;
    bone_dq.quat = float4(gpu_dq1.quat[1], gpu_dq1.quat[2], gpu_dq1.quat[3], gpu_dq1.quat[0]);
    bone_dq.trans = float4(gpu_dq1.trans[1], gpu_dq1.trans[2], gpu_dq1.trans[3], gpu_dq1.trans[0]);
    bone_dq.scale = float4x4(
        float4(gpu_dq1.scale[0][0], gpu_dq1.scale[0][1], gpu_dq1.scale[0][2], gpu_dq1.scale[0][3]),
        float4(gpu_dq1.scale[1][0], gpu_dq1.scale[1][1], gpu_dq1.scale[1][2], gpu_dq1.scale[1][3]),
        float4(gpu_dq1.scale[2][0], gpu_dq1.scale[2][1], gpu_dq1.scale[2][2], gpu_dq1.scale[2][3]),
        float4(
            gpu_dq1.scale[3][0], gpu_dq1.scale[3][1], gpu_dq1.scale[3][2], gpu_dq1.scale[3][3]));
    bone_dq.scale_weight = gpu_dq1.scale_weight;
    bone_dq.quat_weight = 1.0;
    accumulate_dual_quat_pivot(dq_sum, bone_dq, P_rest, weights[1]);
    total_weight += weights[1];
  }

  /* Bone 2 */
  if (weights[2] > 0.0 && bone_idx[2] != 0xFFFFu) {
    DualQuat bone_dq;
    bone_dq.quat = float4(gpu_dq2.quat[1], gpu_dq2.quat[2], gpu_dq2.quat[3], gpu_dq2.quat[0]);
    bone_dq.trans = float4(gpu_dq2.trans[1], gpu_dq2.trans[2], gpu_dq2.trans[3], gpu_dq2.trans[0]);
    bone_dq.scale = float4x4(
        float4(gpu_dq2.scale[0][0], gpu_dq2.scale[0][1], gpu_dq2.scale[0][2], gpu_dq2.scale[0][3]),
        float4(gpu_dq2.scale[1][0], gpu_dq2.scale[1][1], gpu_dq2.scale[1][2], gpu_dq2.scale[1][3]),
        float4(gpu_dq2.scale[2][0], gpu_dq2.scale[2][1], gpu_dq2.scale[2][2], gpu_dq2.scale[2][3]),
        float4(
            gpu_dq2.scale[3][0], gpu_dq2.scale[3][1], gpu_dq2.scale[3][2], gpu_dq2.scale[3][3]));
    bone_dq.scale_weight = gpu_dq2.scale_weight;
    bone_dq.quat_weight = 1.0;
    accumulate_dual_quat_pivot(dq_sum, bone_dq, P_rest, weights[2]);
    total_weight += weights[2];
  }

  /* Bone 3 */
  if (weights[3] > 0.0 && bone_idx[3] != 0xFFFFu) {
    DualQuat bone_dq;
    bone_dq.quat = float4(gpu_dq3.quat[1], gpu_dq3.quat[2], gpu_dq3.quat[3], gpu_dq3.quat[0]);
    bone_dq.trans = float4(gpu_dq3.trans[1], gpu_dq3.trans[2], gpu_dq3.trans[3], gpu_dq3.trans[0]);
    bone_dq.scale = float4x4(
        float4(gpu_dq3.scale[0][0], gpu_dq3.scale[0][1], gpu_dq3.scale[0][2], gpu_dq3.scale[0][3]),
        float4(gpu_dq3.scale[1][0], gpu_dq3.scale[1][1], gpu_dq3.scale[1][2], gpu_dq3.scale[1][3]),
        float4(gpu_dq3.scale[2][0], gpu_dq3.scale[2][1], gpu_dq3.scale[2][2], gpu_dq3.scale[2][3]),
        float4(
            gpu_dq3.scale[3][0], gpu_dq3.scale[3][1], gpu_dq3.scale[3][2], gpu_dq3.scale[3][3]));
    bone_dq.scale_weight = gpu_dq3.scale_weight;
    bone_dq.quat_weight = 1.0;
    accumulate_dual_quat_pivot(dq_sum, bone_dq, P_rest, weights[3]);
    total_weight += weights[3];
  }

  float3 P_final;
  float3 N_final;
  float3 T_final;

  if (total_weight > 0.0) {
    /* Normalize accumulated dual quaternion by total weight. */
    dq_sum.quat_weight = total_weight;
    DualQuat normalized_dq = normalize_dual_quat(dq_sum);

    /* Apply dual quaternion transformation to vertex, normal, and tangent. */
    P_final = transform_point_dual_quat(P_rest, normalized_dq);
    N_final = normalize(transform_normal_dual_quat(N_rest, normalized_dq));
    T_final = normalize(transform_normal_dual_quat(T_rest.xyz, normalized_dq));
  }
  else {
    P_final = P_rest;
    N_final = N_rest;
    T_final = T_rest.xyz;
  }

  out_skinned_pos[gid] = float4(P_final, 1.0);
  out_skinned_nor[gid] = float4(N_final, 0.0);
  out_skinned_tan[gid] = float4(T_final, T_rest.w);
}
