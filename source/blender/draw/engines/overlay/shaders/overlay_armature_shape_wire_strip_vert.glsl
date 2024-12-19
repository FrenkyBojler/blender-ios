/* SPDX-FileCopyrightText: 2019-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "common_view_clipping_lib.glsl"
#include "common_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "select_lib.glsl"

void main()
{
  select_id_set(in_select_buf[gl_InstanceID]);

  mat4 inst_obmat = data_buf[gl_InstanceID];

  vec4 bone_color, state_color;
  mat4 model_mat = extract_matrix_packed_data(inst_obmat, state_color, bone_color);

  vec3 world_pos = (model_mat * vec4(pos, 1.0)).xyz;
  gl_Position = point_world_to_ndc(world_pos);

  finalColor.rgb = mix(state_color.rgb, bone_color.rgb, 0.5);
  finalColor.a = 1.0;
  /* Because the packing clamps the value, the wire width is passed in compressed. */
  wire_width = bone_color.a * WIRE_WIDTH_COMPRESSION;

  view_clipping_distances(world_pos);
}
