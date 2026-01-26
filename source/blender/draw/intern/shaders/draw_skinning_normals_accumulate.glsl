/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_skinning_infos.hh"

COMPUTE_SHADER_CREATE_INFO(draw_skinning_normals_accumulate)

/* Newell's method accumulates Area-Weighted cross products */
void add_newell_cross_v3_v3v3(inout float3 n, float3 v_prev, float3 v_curr)
{
  n[0] += (v_prev[1] - v_curr[1]) * (v_prev[2] + v_curr[2]);
  n[1] += (v_prev[2] - v_curr[2]) * (v_prev[0] + v_curr[0]);
  n[2] += (v_prev[0] - v_curr[0]) * (v_prev[1] + v_curr[1]);
}

void find_vertex_in_face(uint face_start,
                         uint face_size,
                         uint vertex_index,
                         out uint curr,
                         out uint next,
                         out uint prev)
{
  /* Fallback defaults */
  curr = 0;
  next = 1;
  prev = face_size - 1;

  for (uint i = 0; i < face_size; i++) {
    uint corner_vert = corner_verts_buf[face_start + i];
    if (corner_vert == vertex_index) {
      curr = i;
      next = (i + 1) % face_size;
      prev = (i + face_size - 1) % face_size;
      return;
    }
  }
}

float3 safe_normalize(float3 v)
{
  float len_sq = dot(v, v);
  if (len_sq > 1e-12f) {
    return v * inversesqrt(len_sq);
  }
  return float3(0.0f);
}

void main()
{
  uint vertex_index = gl_GlobalInvocationID.x;
  if (vertex_index >= uint(vertex_count)) {
    return;
  }

  uint first_adjacent_face_offset = face_adjacency_offsets[vertex_index];
  uint number_of_adjacent_faces = face_adjacency_offsets[vertex_index + 1] -
                                  first_adjacent_face_offset;

  float3 accumulated_normal = float3(0.0f);

  for (uint i = 0; i < number_of_adjacent_faces; i++) {
    uint packed_face_info = face_adjacency_lists[first_adjacent_face_offset + i];
    uint face_start = packed_face_info & 0x00FFFFFFu;
    uint face_size = (packed_face_info >> 24) & 0xFFu;

    /* 1. Compute Face Normal (Newell's Method) */
    /* CRITICAL: This vector is NOT normalized. Its length is 2 * Face Area. */
    float3 face_normal = float3(0.0f);

    /* Read positions directly (N-Gon support) */
    for (uint j = 0; j < face_size; j++) {
      uint j_next = (j + 1) % face_size;
      float3 v_curr = skinned_pos_buf[face_start + j].xyz;
      float3 v_next = skinned_pos_buf[face_start + j_next].xyz;
      add_newell_cross_v3_v3v3(face_normal, v_curr, v_next);
    }

    /* FIX: Normalize here to remove Area Weighting.
     * Now it represents direction only. */
    float3 face_normal_normalized = safe_normalize(face_normal);

    /* 2. Compute Angle */
    uint curr_idx, next_idx, prev_idx;
    find_vertex_in_face(face_start, face_size, vertex_index, curr_idx, next_idx, prev_idx);

    float3 v_curr = skinned_pos_buf[face_start + curr_idx].xyz;
    float3 v_prev = skinned_pos_buf[face_start + prev_idx].xyz;
    float3 v_next = skinned_pos_buf[face_start + next_idx].xyz;

    /* Vectors pointing OUT from current vertex to neighbors */
    float3 edvec_prev = safe_normalize(v_prev - v_curr);
    float3 edvec_next = safe_normalize(v_next - v_curr);

    /* Calculate Angle */
    float dot_prod = dot(edvec_prev, edvec_next);
    float fac = acos(clamp(dot_prod, -1.0f, 1.0f));

    /* 3. Accumulate: Face Normal (Direction) * Angle (fac) */
    /* Use the normalized version here */
    accumulated_normal += face_normal_normalized * fac;
  }

  /* Final Normalize */
  float3 normal = safe_normalize(accumulated_normal);

  /* Fallback for degenerate geometry */
  if (dot(normal, normal) == 0.0f) {
    normal = float3(0.0f, 0.0f, 1.0f);
  }

  vert_normals_buf[vertex_index] = float4(normal, 0.0f);
}
