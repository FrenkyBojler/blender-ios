/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compute shader for armature modifier deformation using Dual Quaternion Skinning.
 * 
 * Implements GPU-accelerated dual quaternion skinning with shared memory optimization
 * for bone transformations. Uses pivot-based accumulation to fix scale artifacts.
 * Dual quaternions are pre-transformed to target object coordinate space on the CPU.
 */

#include "draw_skinning_infos.hh"
#include "draw_math_quat_lib.glsl"

COMPUTE_SHADER_CREATE_INFO(draw_armature_skinning_dqs_comp)

/* Shared memory hash table for dual quaternion deduplication within workgroups. */
#define HASH_SIZE 128u
#define HASH_MASK (HASH_SIZE - 1u)
#define EMPTY_SLOT 0xFFFFFFFFu

shared uint s_hash_keys[HASH_SIZE];
shared uint s_hash_pos[HASH_SIZE];
shared uint s_unique_count;
shared GPUDualQuat s_shared_dqs[HASH_SIZE];
shared uint s_compact_to_bone[HASH_SIZE];

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

uint hash_func(uint key) {
  key ^= key >> 16;
  key *= 0x85ebca6bu;
  key ^= key >> 13;
  key *= 0xc2b2ae35u;
  key ^= key >> 16;
  return key & HASH_MASK;
}

/* Linear probing hash table insertion with atomic compare-and-swap for thread safety. */
uint hash_insert(uint key)
{
  uint probe = hash_func(key);
  for (uint iter = 0u; iter < HASH_SIZE; ++iter) {
    uint prev = atomicCompSwap(s_hash_keys[probe], EMPTY_SLOT, key);
    if (prev == EMPTY_SLOT || prev == key) {
      return probe;
    }
    probe = (probe + 1u) & HASH_MASK;
  }
  return EMPTY_SLOT;
}

uint hash_find(uint key)
{
  uint probe = hash_func(key);
  for (uint iter = 0u; iter < HASH_SIZE; ++iter) {
    uint v = s_hash_keys[probe];
    if (v == key) return probe;
    if (v == EMPTY_SLOT) return EMPTY_SLOT;
    probe = (probe + 1u) & HASH_MASK;
  }
  return EMPTY_SLOT;
}

void main()
{
  uint gid = gl_GlobalInvocationID.x;
  if (gid >= uint(vertex_count)) return;
  uint lid = gl_LocalInvocationID.x;
  uint lsize = gl_WorkGroupSize.x;

  /* Initialize shared memory hash table cooperatively across workgroup threads. */
  for (uint i = lid; i < HASH_SIZE; i += lsize) {
    s_hash_keys[i] = EMPTY_SLOT;
    s_hash_pos[i] = EMPTY_SLOT;
  }
  if (lid == 0u) {
    s_unique_count = 0u;
  }
  memoryBarrierShared();
  barrier();

  uint idx_u0 = indices_buf[gid].x;
  uint idx_u1 = indices_buf[gid].y;
  uvec4 bone_idx = unpack_indices_from_two_uints(idx_u0, idx_u1);

  /* Insert unique bone indices into hash table to minimize global memory access. */
  for (int k = 0; k < 4; ++k) {
    uint bi = bone_idx[k];

    if (bi != 0xFFFFu) {
      uint probe = hash_insert(bi);
    }
  }

  memoryBarrierShared();
  barrier();

  /* Build compact mapping from hash table positions to bone indices. */
  for (uint i = lid; i < HASH_SIZE; i += lsize) {
    if (s_hash_keys[i] != EMPTY_SLOT) {
      uint compact_idx = atomicAdd(s_unique_count, 1u);
      s_hash_pos[i] = compact_idx;
      s_compact_to_bone[compact_idx] = s_hash_keys[i];
    }
  }

  memoryBarrierShared();
  barrier();

  /* Load dual quaternions into shared memory using compact indices. */
  for (uint i = lid; i < s_unique_count; i += lsize) {
    uint bone_idx = s_compact_to_bone[i];
    s_shared_dqs[i] = bonedq_buf[bone_idx];
  }

  memoryBarrierShared();
  barrier();

  uint w_u0 = weights_buf[gid].x;
  uint w_u1 = weights_buf[gid].y;

  if (w_u0 == 0u && w_u1 == 0u) {
    vec4 P_rest = pos_buf[gid];
    vec2 N_packed = nor_buf[gid];
    vec3 N_rest = unpack_octahedral(N_packed);
    out_skinned_pos[gid] = P_rest;
    out_skinned_nor[gid] = vec4(N_rest, 0.0f);
    return;
  }

  vec4 weights = unpack_weights_from_two_uints(w_u0, w_u1);

  vec3 P_rest = vec3(pos_buf[gid]);
  vec2 N_packed = nor_buf[gid];
  vec3 N_rest = unpack_octahedral(N_packed);

  /* Initialize dual quaternion accumulator. */
  DualQuat dq_sum;
  dq_sum.quat = vec4(0.0, 0.0, 0.0, 0.0);
  dq_sum.trans = vec4(0.0, 0.0, 0.0, 0.0);
  dq_sum.scale = mat4(0.0);
  dq_sum.scale_weight = 0.0;
  dq_sum.quat_weight = 0.0;

  float total_weight = 0.0;

  /* Accumulate weighted dual quaternions from influencing bones. */
  for (int k = 0; k < 4; ++k) {
    uint bi = bone_idx[k];
    float w = weights[k];

    if (w > 0.0 && bi != 0xFFFFu) {
      uint probe = hash_find(bi);
      if (probe != EMPTY_SLOT) {
        uint local_idx = s_hash_pos[probe];
        GPUDualQuat gpu_dq = s_shared_dqs[local_idx];

        /* Convert GPU dual quaternion format to shader format.
         * Quaternion components are reordered from [w,x,y,z] to [x,y,z,w]. */
        DualQuat bone_dq;
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

        /* Use vertex position as pivot point to neutralize scale artifacts. */
        accumulate_dual_quat_pivot(dq_sum, bone_dq, P_rest, w);
        total_weight += w;
      }
    }
  }

  vec3 P_final;
  vec3 N_final;

  if (total_weight > 0.0) {
    /* Normalize accumulated dual quaternion by total weight. */
    dq_sum.quat_weight = total_weight;
    DualQuat normalized_dq = normalize_dual_quat(dq_sum);

    /* Apply dual quaternion transformation to vertex and normal. */
    P_final = transform_point_dual_quat(P_rest, normalized_dq);
    N_final = normalize(transform_normal_dual_quat(N_rest, normalized_dq));
  }
  else {
    /* No bone influences - preserve rest pose. */
    P_final = P_rest;
    N_final = N_rest;
  }

  out_skinned_pos[gid] = vec4(P_final, 1.0);
  out_skinned_nor[gid] = vec4(N_final, 0.0);
}
