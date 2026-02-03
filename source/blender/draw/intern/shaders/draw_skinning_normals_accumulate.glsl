/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_skinning_infos.hh"

COMPUTE_SHADER_CREATE_INFO(draw_skinning_normals_accumulate)

void add_newell_cross_v3_v3v3(inout float3 n, float3 v_prev, float3 v_curr)
{
  n[0] += (v_prev[1] - v_curr[1]) * (v_prev[2] + v_curr[2]);
  n[1] += (v_prev[2] - v_curr[2]) * (v_prev[0] + v_curr[0]);
  n[2] += (v_prev[0] - v_curr[0]) * (v_prev[1] + v_curr[1]);
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

    if (face_size < 3) {
      continue;
    }

    float3 face_normal = float3(0.0f);
    uint curr_idx = 0;
    float3 v_curr, v_prev, v_next;
    bool found_vertex = false;

    for (uint j = 0; j < face_size; j++) {
      uint corner_idx = face_start + j;
      uint corner_vert = corner_verts_buf[corner_idx];
      float3 v_j = skinned_pos_buf[corner_idx].xyz;

      /* Check if this is our vertex */
      if (corner_vert == vertex_index) {
        curr_idx = j;
        v_curr = v_j;
        found_vertex = true;
      }

      uint j_next = (j + 1) % face_size;
      float3 v_j_next = skinned_pos_buf[face_start + j_next].xyz;
      add_newell_cross_v3_v3v3(face_normal, v_j, v_j_next);
    }

    if (!found_vertex) {
      continue;
    }

    float face_len_sq = dot(face_normal, face_normal);
    if (face_len_sq < 1e-12f) {
      continue;
    }
    float3 face_normal_normalized = face_normal * inversesqrt(face_len_sq);

    uint prev_idx = (curr_idx + face_size - 1) % face_size;
    uint next_idx = (curr_idx + 1) % face_size;
    v_prev = skinned_pos_buf[face_start + prev_idx].xyz;
    v_next = skinned_pos_buf[face_start + next_idx].xyz;

    float3 edvec_prev = v_prev - v_curr;
    float3 edvec_next = v_next - v_curr;

    float prev_len_sq = dot(edvec_prev, edvec_prev);
    float next_len_sq = dot(edvec_next, edvec_next);

    if (prev_len_sq > 1e-12f && next_len_sq > 1e-12f) {
      edvec_prev *= inversesqrt(prev_len_sq);
      edvec_next *= inversesqrt(next_len_sq);

      float dot_prod = dot(edvec_prev, edvec_next);
      float fac = acos(clamp(dot_prod, -1.0f, 1.0f));

      accumulated_normal += face_normal_normalized * fac;
    }
  }

  if (dot(accumulated_normal, accumulated_normal) == 0.0f) {
    accumulated_normal = float3(0.0f, 0.0f, 1.0f);
  }

  vert_normals_buf[vertex_index] = float4(accumulated_normal, 0.0f);
}
