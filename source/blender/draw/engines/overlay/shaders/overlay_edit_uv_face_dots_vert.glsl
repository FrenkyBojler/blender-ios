/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_edit_mode_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_edit_uv_face_dots)

#include "draw_model_lib.glsl"
#include "draw_view_lib.glsl"

void main()
{
  float3 world_pos = float3(au, 0.0f);
  gl_Position = drw_point_world_to_homogenous(world_pos);

  bool is_selected = (flag & FACE_UV_SELECT) != 0u;
  bool is_mirrored = (flag & FACE_MIRRORED_SELECT) != 0u;

  final_color = (is_selected) ? theme.colors.facedot : float4(theme.colors.wire.rgb, 1.0f);
  if (!is_selected && is_mirrored) {
    final_color = theme.colors.face_mirror_selection;
  }
  gl_PointSize = dot_size;
}
