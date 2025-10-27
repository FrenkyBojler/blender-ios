/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  const float[2] vertex_offsets = { -1.0f, 1.0f };

  int vertex_idx = gl_VertexID % 2; // vertex 0, vertex 1, vertex 0, vertex 1
  int line_idx = gl_VertexID / 2;   // vertex 0, vertex 0, vertex 1, vertex 1
  
  // int line_id   = gl_VertexID / 2;

  float3 vert_pos = float3(0.0f, offsets[gl_VertexID % 2], 0.0f);
  // float3 real_pos = drw_view_position() + vert_pos;

  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vert_pos, 1.0f));
}