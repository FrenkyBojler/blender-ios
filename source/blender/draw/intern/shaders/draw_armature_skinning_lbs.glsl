/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Compute shader for armature modifier deformation using Linear Blend Skinning.
 *
 */

#include "draw_skinning_infos.hh"

COMPUTE_SHADER_CREATE_INFO(draw_armature_skinning_comp)

/* Shared memory hash table for bone matrix deduplication within workgroups. */
#define HASH_SIZE 128u
#define HASH_MASK (HASH_SIZE - 1u)
#define EMPTY_SLOT 0xFFFFFFFFu

shared uint s_hash_keys[HASH_SIZE];
shared uint s_hash_pos[HASH_SIZE];
shared uint s_unique_count;
shared mat4 s_shared_mats[HASH_SIZE];
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

  for (int k = 0; k < 4; ++k) {
    uint bi = bone_idx[k];
    if (bi != 0xFFFFu) {
      hash_insert(bi);
    }
  }

  memoryBarrierShared();
  barrier();

  if (lid == 0u) {
    uint compact_idx = 0u;
    for (uint i = 0u; i < HASH_SIZE; ++i) {
      if (s_hash_keys[i] != EMPTY_SLOT) {
        s_hash_pos[i] = compact_idx;
        s_compact_to_bone[compact_idx] = s_hash_keys[i];
        compact_idx++;
      }
    }
    s_unique_count = compact_idx;
  }

  memoryBarrierShared();
  barrier();

  /* Load bone matrices into shared memory using compact indices. */
  for (uint i = lid; i < s_unique_count; i += lsize) {
    uint bone_idx = s_compact_to_bone[i];
    s_shared_mats[i] = bonemat_buf[bone_idx];
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
  vec4 P_rest_f = vec4(P_rest, 1.0f);
  vec2 N_packed = nor_buf[gid];
  vec3 N_rest = unpack_octahedral(N_packed);

  /* Accumulate transformation deltas for numerical stability. */
  vec3 co_accum = vec3(0.0f);
  vec3 N_accum = vec3(0.0f);
  float total_weight = 0.0f;

  for (int k = 0; k < 4; ++k) {
    uint bi = bone_idx[k];
    float w = weights[k];

    if (w > 0.0f && bi != 0xFFFFu) {
      uint probe = hash_find(bi);
      if (probe != EMPTY_SLOT) {
        uint local_idx = s_hash_pos[probe];
        mat4 bm = s_shared_mats[local_idx];

        /* Accumulate position delta from rest pose. */
        vec3 P_trans = (bm * P_rest_f).xyz;
        vec3 P_eval = P_trans - P_rest;
        co_accum += P_eval * w;

        /* Accumulate normal delta using 3x3 rotation part. */
        vec3 transformed_nor = mat3(bm) * N_rest;
        vec3 N_eval = transformed_nor - N_rest;
        N_accum += N_eval * w;

        total_weight += w;
      }
    }
  }

  vec3 P_final = P_rest + co_accum;
  vec3 N_final = normalize(N_rest + N_accum);

  out_skinned_pos[gid] = vec4(P_final, 1.0f);
  out_skinned_nor[gid] = vec4(N_final, 0.0f);
}