/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compute shader for armature modifier deformation using Dual Quaternion Skinning.
 */

#include "draw_skinning_infos.hh"
#include "draw_math_quat_lib.glsl"

COMPUTE_SHADER_CREATE_INFO(draw_armature_skinning_dqs)

vec2 unpack_weights_from_uint(uint x) {
  const float inv65535 = 1.0f / 65535.0f;
  uint w0 = x & 0xFFFFu;
  uint w1 = (x >> 16) & 0xFFFFu;
  return vec2(float(w0) * inv65535, float(w1) * inv65535);
}

vec4 unpack_weights_from_two_uints(uint a, uint b) {
  vec2 p0 = unpack_weights_from_uint(a);
  vec2 p1 = unpack_weights_from_uint(b);
  return vec4(p0.x, p0.y, p1.x, p1.y);
}

uvec2 unpack_indices_from_uint(uint x) {
  uint i0 = x & 0xFFFFu;
  uint i1 = (x >> 16) & 0xFFFFu;
  return uvec2(i0, i1);
}

uvec4 unpack_indices_from_two_uints(uint a, uint b) {
  uvec2 p0 = unpack_indices_from_uint(a);
  uvec2 p1 = unpack_indices_from_uint(b);
  return uvec4(p0.x, p0.y, p1.x, p1.y);
}

vec2 sign_not_zero(vec2 v) {
  return vec2(v.x >= 0.0f ? 1.0f : -1.0f, v.y >= 0.0f ? 1.0f : -1.0f);
}

vec3 unpack_octahedral(vec2 p)
{
  vec3 v = vec3(p.x, p.y, 1.0f - abs(p.x) - abs(p.y));
  if (v.z < 0.0f) {
    v.xy = (1.0f - abs(v.yx)) * sign_not_zero(v.xy);
  }
  return normalize(v);
}

/* Helper function to convert GPU dual quaternion to shader format */
DualQuat convert_gpu_dualquat(GPUDualQuat gpu_dq) {
  DualQuat bone_dq;
  /* Quaternion components reordered from [w,x,y,z] to [x,y,z,w] */
  bone_dq.quat = vec4(gpu_dq.quat[1], gpu_dq.quat[2], gpu_dq.quat[3], gpu_dq.quat[0]);
  bone_dq.trans = vec4(gpu_dq.trans[1], gpu_dq.trans[2], gpu_dq.trans[3], gpu_dq.trans[0]);
  bone_dq.scale = mat4(
    vec4(gpu_dq.scale[0][0], gpu_dq.scale[0][1], gpu_dq.scale[0][2], gpu_dq.scale[0][3]),
    vec4(gpu_dq.scale[1][0], gpu_dq.scale[1][1], gpu_dq.scale[1][2], gpu_dq.scale[1][3]),
    vec4(gpu_dq.scale[2][0], gpu_dq.scale[2][1], gpu_dq.scale[2][2], gpu_dq.scale[2][3]),
    vec4(gpu_dq.scale[3][0], gpu_dq.scale[3][1], gpu_dq.scale[3][2], gpu_dq.scale[3][3])
  );
  bone_dq.scale_weight = gpu_dq.scale_weight;
  bone_dq.quat_weight = 1.0;
  return bone_dq;
}

void main()
{
  uint gid = gl_GlobalInvocationID.x;
  if (gid >= uint(vertex_count)) {
    return;
  }

  /* Load bone indices and weights */
  uint idx_u0 = indices_buf[gid].x;
  uint idx_u1 = indices_buf[gid].y;
  uvec4 bone_idx = unpack_indices_from_two_uints(idx_u0, idx_u1);

  uint w_u0 = weights_buf[gid].x;
  uint w_u1 = weights_buf[gid].y;

  /* Early exit for unweighted vertices */
  if (w_u0 == 0u && w_u1 == 0u) {
    vec4 P_rest = pos_buf[gid];
    vec2 N_packed = nor_buf[gid];
    vec3 N_rest = unpack_octahedral(N_packed);
    vec4 T_rest = tan_buf[gid];
    out_skinned_pos[gid] = P_rest;
    out_skinned_nor[gid] = vec4(N_rest, 0.0f);
    out_skinned_tan[gid] = T_rest;
    return;
  }

  vec4 weights = unpack_weights_from_two_uints(w_u0, w_u1);

  /* Load vertex data */
  vec3 P_rest = vec3(pos_buf[gid]);
  vec2 N_packed = nor_buf[gid];
  vec3 N_rest = unpack_octahedral(N_packed);
  vec4 T_rest = tan_buf[gid];

  GPUDualQuat gpu_dq0 = (bone_idx[0] != 0xFFFFu) ? bonedq_buf[bone_idx[0]] : GPUDualQuat(vec4(1,0,0,0), vec4(0,0,0,0), mat4(1.0), 0.0);
  GPUDualQuat gpu_dq1 = (bone_idx[1] != 0xFFFFu) ? bonedq_buf[bone_idx[1]] : GPUDualQuat(vec4(1,0,0,0), vec4(0,0,0,0), mat4(1.0), 0.0);
  GPUDualQuat gpu_dq2 = (bone_idx[2] != 0xFFFFu) ? bonedq_buf[bone_idx[2]] : GPUDualQuat(vec4(1,0,0,0), vec4(0,0,0,0), mat4(1.0), 0.0);
  GPUDualQuat gpu_dq3 = (bone_idx[3] != 0xFFFFu) ? bonedq_buf[bone_idx[3]] : GPUDualQuat(vec4(1,0,0,0), vec4(0,0,0,0), mat4(1.0), 0.0);

  /* Initialize dual quaternion accumulator */
  DualQuat dq_sum;
  dq_sum.quat = vec4(0.0, 0.0, 0.0, 0.0);
  dq_sum.trans = vec4(0.0, 0.0, 0.0, 0.0);
  dq_sum.scale = mat4(0.0);
  dq_sum.scale_weight = 0.0;
  dq_sum.quat_weight = 0.0;

  float total_weight = 0.0;

  
  /* Bone 0 */
  if (weights[0] > 0.0f && bone_idx[0] != 0xFFFFu) {
    DualQuat bone_dq = convert_gpu_dualquat(gpu_dq0);
    accumulate_dual_quat_pivot(dq_sum, bone_dq, P_rest, weights[0]);
    total_weight += weights[0];
  }

  if (weights[1] > 0.0f && bone_idx[1] != 0xFFFFu) {
    DualQuat bone_dq = convert_gpu_dualquat(gpu_dq1);
    accumulate_dual_quat_pivot(dq_sum, bone_dq, P_rest, weights[1]);
    total_weight += weights[1];
  }

  if (weights[2] > 0.0f && bone_idx[2] != 0xFFFFu) {
    DualQuat bone_dq = convert_gpu_dualquat(gpu_dq2);
    accumulate_dual_quat_pivot(dq_sum, bone_dq, P_rest, weights[2]);
    total_weight += weights[2];
  }

  if (weights[3] > 0.0f && bone_idx[3] != 0xFFFFu) {
    DualQuat bone_dq = convert_gpu_dualquat(gpu_dq3);
    accumulate_dual_quat_pivot(dq_sum, bone_dq, P_rest, weights[3]);
    total_weight += weights[3];
  }

  vec3 P_final;
  vec3 N_final;
  vec3 T_final;

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

  out_skinned_pos[gid] = vec4(P_final, 1.0);
  out_skinned_nor[gid] = vec4(N_final, 0.0);
  out_skinned_tan[gid] = vec4(T_final, T_rest.w);
}
