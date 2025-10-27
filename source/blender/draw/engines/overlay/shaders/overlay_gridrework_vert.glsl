/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  int line_idx = (gl_VertexID / 2) % int(grid_buf.num_lines);     // vertex 0, vertex 0, vertex 1, vertex 1
  int line_start = -int(grid_buf.num_lines >> 1);

  float3 vert_pos = float3(line_start + line_idx, line_start, 0.0f);
  
  // If not start vertex, flip y-coord for other side of line
  int vert_idx = gl_VertexID % 2;
  vert_pos.y = (vert_idx == 0) ? vert_pos.y : -vert_pos.y;

  // If not x-direction, flip x- and -ycoords for y-direction
  int dir_idx  = gl_VertexID / int(grid_buf.num_lines * 2);
  vert_pos.xy = (dir_idx == 0) ? vert_pos.xy : vert_pos.yx;

  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vert_pos, 1.0f));
}