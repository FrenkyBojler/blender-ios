/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  // TODO; parameterize; 4 is levels and 2 is directions for now
  // Extract line data from vertex id
  uint idx = gl_VertexID;
  uint line_side = idx & 0x01;                        // LSB indicates start/end vertex
  idx = idx >> 1;
  uint line_idx = idx % grid_buf.num_lines;           // index of line, from x-min to x-max
  uint line_dir = (idx / grid_buf.num_lines) % 2;     // direction of line, x or y
  uint line_lvl = (idx / grid_buf.num_lines / 2) % 4; // lvl of line on grid, wraps around

  // Determine distance to floor plane through camera center
  float t = drw_view_position().z / drw_view_forward().z;

  // Rotate levels dependent on camera distance
  // TODO; this is a total hack for testing
  // line_lvl += log2(t);

  // Determine line scale for current level
  // TODO; 1.f should become unit size 
  float line_scale = (1.f) * (1 << line_lvl);

  // Offset distance to most outer line
  float line_start = -line_scale * float(grid_buf.num_lines >> 1);

  // Determine vertex position
  // If not start vertex, flip y-coord for other side of line
  // If not x-direction, flip x- and -ycoords for y-direction
  float3 vert_pos = float3(line_start + line_scale * line_idx, line_start, 0.0f);
  vert_pos.y = bool(line_side) ? vert_pos.y : -vert_pos.y;
  vert_pos.xy = bool(line_dir) ? vert_pos.xy : vert_pos.yx;

  // Add camera offset; grid moves with camera
  float3 pos_on_floor = drw_view_position() + t * -drw_view_forward();
  pos_on_floor = float3(pos_on_floor.xy, 0);
  vert_pos += floor(pos_on_floor / line_scale) * line_scale;

  // TODO; remove; offset lines vertically to see overlaps
  // vert_pos.z -= 0.05 * line_lvl;

  // Vertex outputs
  local_pos = vert_pos;
  local_lvl = line_lvl;
  
  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vert_pos, 1.0f));
}