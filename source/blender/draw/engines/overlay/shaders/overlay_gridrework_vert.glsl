/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"

#define GRID_SUBDIVS    10 /* Subdivision is a factor of 10 between levels. */
#define GRID_LEVELS      4 /* The grid supports 3 hardcoded levels of hierarchy. */

void main()
{
  uint line_idx = gl_VertexID;
  
  /* Every pair of consecutive vertices forms a line, indicated by bit 0 of gl_VertexID. */
  uint line_side = line_idx & 0x1;
  line_idx = line_idx >> 1;

  /* Every pair of consecutive lines alternates x/y direction, indicated by bit 1 of gl_VertexID. */
  uint line_dir = line_idx & 0x1;
  line_idx = line_idx >> 1;

  /* Line counts per level are packed, so we unpack and find a line's actual level/index by
   * doing a simple cumulative sum. */
  uint4 levels = unpackUint8x4(grid_buf.num_lines_per_level_pack);
  int line_lvl;
  int num_lines;
  for (int i = 0; i < GRID_LEVELS; ++i) {
    num_lines = int(levels[i]);
    if (line_idx < num_lines) {
      line_lvl = i;
      break;
    } else {
      line_idx -= num_lines;
    }
  }

  /* We next calculate the vertex position at the start/end of a line. */
  float vert_x = -max(float(num_lines >> 1), 1.f);
  float vert_y = vert_x + float(line_idx);
  float3 vert_pos = float3(vert_y, vert_x, 0.0f);
  /* If this isn't the start vertex, flip the y-coord to define the end vertex. */
  vert_pos.y = select(vert_pos.y, -vert_pos.y, line_side);
  /* If this isn't the x-direction, flip coordinates to define the y-direction. */
  vert_pos.xy = select(vert_pos.xy, vert_pos.yx, line_dir);

  /* For fragment shader; output vertex position in [-1, 1]. */
  frag_xy = vert_pos.xy / vert_x;
  
  /* We now adjust the grid level dependent on the distance to a point on the floor plane.
   * First; determine the distance to this point. */
  float abs_cos_theta = abs(drw_view_forward().z); 
  float abs_z = abs(drw_view_position().z);
  float t = mix(abs_z / abs_cos_theta, abs_z, 1.0f - abs_cos_theta);

  /* Then, determine the level adjustment. */
  float line_lvl_offset = log2(t) / log2(float(GRID_SUBDIVS)); /* log10(t) = log2(t) / log2(10) */
  line_lvl_offset = max(-0.67f, line_lvl_offset - 2.f); /* Subtraction ensures we always show a sublevel. */

  /* To fade the grid levels in/out smoothly, we output a fade factor to fragment. */
  if (line_lvl < GRID_LEVELS - 1) {
    frag_level = (float(line_lvl) + 1.f - fract(line_lvl_offset)) / float(GRID_LEVELS - 1);
  } else {
    frag_level = 1.f;
  }

  /* Finally, add the rounded-up level adjustment to the actual line level */
  line_lvl = line_lvl + int(ceil(line_lvl_offset));

  /* Each level of the grid is an order of magnitude larger than the previous level */
  float line_scale = pow(float(GRID_SUBDIVS), float(line_lvl));
  vert_pos *= line_scale;
  
  /* The grid moves with the camera in increments so as to go unnoticed. */
  float3 pos_on_floor = float3(drw_view_position().xy + t * -drw_view_forward().xy, 0);
  vert_pos += round(pos_on_floor / line_scale) * (line_scale);

  local_pos = vert_pos;
  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vert_pos, 1.0f));
}