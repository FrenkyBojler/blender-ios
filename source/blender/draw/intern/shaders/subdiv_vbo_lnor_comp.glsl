/* SPDX-FileCopyrightText: 2021-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "subdiv_lib.glsl"

COMPUTE_SHADER_CREATE_INFO(subdiv_loop_normals)

void main()
{
  /* We execute for each quad. */
  uint quad_index = get_global_invocation_index();
  if (quad_index >= shader_data.total_dispatch_size) {
    return;
  }

  /* The start index of the loop is quad_index * 4. */
  uint start_loop_index = quad_index * 4;

  uint coarse_quad_index = coarse_face_index_from_subdiv_quad_index(quad_index,
                                                                    shader_data.coarse_face_count);

  if ((extra_coarse_face_data[coarse_quad_index] & shader_data.coarse_face_smooth_mask) != 0) {
    /* Face is smooth, use vertex normals. */
    for (int i = 0; i < 4; i++) {
      uint subdiv_vert_index = vert_loop_map[start_loop_index + i];
      vec3 vert_normal = vert_normals[subdiv_vert_index];
      Normal nor;
      nor.x = vert_normal.x;
      nor.y = vert_normal.y;
      nor.z = vert_normal.z;
      output_lnor[start_loop_index + i] = nor;
    }
  }
  else {
    vec3 v0 = positions[start_loop_index + 0];
    vec3 v1 = positions[start_loop_index + 1];
    vec3 v2 = positions[start_loop_index + 2];
    vec3 v3 = positions[start_loop_index + 3];

    vec3 face_normal = vec3(0.0);
    add_newell_cross_v3_v3v3(face_normal, v0, v1);
    add_newell_cross_v3_v3v3(face_normal, v1, v2);
    add_newell_cross_v3_v3v3(face_normal, v2, v3);
    add_newell_cross_v3_v3v3(face_normal, v3, v0);

    face_normal = normalize(face_normal);

    Normal nor;
    nor.x = face_normal.x;
    nor.y = face_normal.y;
    nor.z = face_normal.z;
    for (int i = 0; i < 4; i++) {
      output_lnor[start_loop_index + i] = nor;
    }
  }
}
