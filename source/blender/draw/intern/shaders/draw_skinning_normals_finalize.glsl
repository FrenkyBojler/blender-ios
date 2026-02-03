/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_skinning_infos.hh"

COMPUTE_SHADER_CREATE_INFO(draw_skinning_normals_finalize)

void add_newell_cross_v3_v3v3(inout float3 n, float3 v_prev, float3 v_curr)
{
  n[0] += (v_prev[1] - v_curr[1]) * (v_prev[2] + v_curr[2]);
  n[1] += (v_prev[2] - v_curr[2]) * (v_prev[0] + v_curr[0]);
  n[2] += (v_prev[0] - v_curr[0]) * (v_prev[1] + v_curr[1]);
}

void main()
{
  uint face_index = gl_GlobalInvocationID.x;
  if (face_index >= uint(face_count)) {
    return;
  }

  /* Get face corner range from face_offsets */
  uint start_corner = face_offsets_buf[face_index];
  uint end_corner = face_offsets_buf[face_index + 1];
  uint face_size = end_corner - start_corner;

  /* Check if using flat shading (bit set in sharp_faces_buf means flat shading) */
  uint word_index = face_index / 32u;
  uint bit_index = face_index % 32u;
  bool use_flat_shading = (sharp_faces_buf[word_index] & (1u << bit_index)) != 0u;

  if (!use_flat_shading) {
    /* Smooth shading */
    for (uint i = 0u; i < face_size; i++) {
      uint corner_idx = start_corner + i;
      uint vert_idx = corner_verts_buf[corner_idx];
      float4 vert_normal = vert_normals_buf[vert_idx];
      out_skinned_nor[corner_idx] = vert_normal;
    }
  }
  else {
    /* flat shading*/
    float3 face_normal = float3(0.0f);

    for (uint i = 0u; i < face_size; i++) {
      uint curr_corner = start_corner + i;
      uint next_corner = start_corner + ((i + 1u) % face_size);

      float3 v_curr = skinned_pos_buf[curr_corner].xyz;
      float3 v_next = skinned_pos_buf[next_corner].xyz;

      add_newell_cross_v3_v3v3(face_normal, v_curr, v_next);
    }

    float len_sq = dot(face_normal, face_normal);
    if (len_sq > 1e-8) {
      face_normal *= inversesqrt(len_sq);
    }

    for (uint i = 0u; i < face_size; i++) {
      out_skinned_nor[start_corner + i] = float4(face_normal, 0.0f);
    }
  }
}
