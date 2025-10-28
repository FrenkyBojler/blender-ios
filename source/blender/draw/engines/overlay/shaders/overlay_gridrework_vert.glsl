/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  // Determine index of current line from 0 to num_lines - 1
  int line_idx = (gl_VertexID / 2) % int(grid_buf.num_lines);     // vertex 0, vertex 0, vertex 1, vertex 1

  // Offset distance to most outer line
  // TODO; this should be a fixed distance or the edge of the camera plane
  float line_offset = 1.0f;
  float line_start = -line_offset * float(grid_buf.num_lines >> 1);
  
  float3 vert_pos = float3(line_start + line_offset * line_idx, line_start, 0.0f);

  // If not start vertex, flip y-coord for other side of line
  bool is_first_vert = (gl_VertexID % 2) == 0;
  vert_pos.y = is_first_vert ? vert_pos.y : -vert_pos.y;

  // If not x-direction, flip x- and -ycoords for y-direction
  bool is_x_dir = (gl_VertexID / int(grid_buf.num_lines * 2)) == 0;
  vert_pos.xy = is_x_dir ? vert_pos.xy : vert_pos.yx;

  // Add camera offset; grid moves with camera; we shoot a ray through the floor and move
  // around this point for now
  float t = -drw_view_position().z / -drw_view_forward().z;
  float3 pos_on_floor = drw_view_position() + t * -drw_view_forward();
  pos_on_floor = float3(pos_on_floor.xy, 0);

  vert_pos += floor(pos_on_floor);
  local_pos = vert_pos;
  
  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vert_pos, 1.0f));
}