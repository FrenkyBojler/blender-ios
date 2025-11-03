/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"

// TODO merge with vert definition
#define GRID_DIRECTIONS    2
#define GRID_SUBDIVS      10
#define GRID_LEVEL_OFFSET -2

void main()
{
  // Every pair of consecutive vertices forms a line
  uint idx = gl_VertexID;
  uint line_side = idx & 0x01;                                                            // LSB indicates start/end vertex
  idx = idx >> 1;

  // Extract line data from vertex id
  uint line_idx = (idx % grid_buf.num_lines); // - (grid_buf.num_lines / 2);      
  uint line_dir = (idx / grid_buf.num_lines) % GRID_DIRECTIONS;                       // direction of line, x or y
  int line_lvl = int((idx / grid_buf.num_lines / GRID_DIRECTIONS) % grid_buf.num_levels); // lvl of line on grid, wraps around

  debug_grid_lvl = uint(line_lvl);

  // Discard lines that are present in a level above
  /* if (abs(int(line_idx) - int(grid_buf.num_lines / 2)) % GRID_SUBDIVS == 0 && line_lvl < (GRID_LEVELS - 1)) {
    gl_Position = float4(NAN_FLT);
    return;
  } */
  
  // Determine distance to floor plane through center point
  float abs_cos_theta = abs(drw_view_forward().z); 
  float abs_z = abs(drw_view_position().z);
  float t = abs_z / abs_cos_theta;
  t = mix(t, abs_z, 1.0f - abs_cos_theta); // TODO re-enable

  // Rotate levels dependent on distance to floor plane point
  // Value to offset levels by, dependent on distance to center point
  float line_lvl_mod = /* max(0.f, */ log2(t) / log2(float(GRID_SUBDIVS)); // log10(t)
  
  // Grid levels fade in smoothly, passed through a sigmoidal
  float temp = 1.f;
  if (line_lvl < (grid_buf.num_levels - 1)) {
    temp = (float(line_lvl) + 1.f - fract(line_lvl_mod)) / float(grid_buf.num_levels - 1);
  } else if (line_lvl == (grid_buf.num_levels - 1)) {
    temp = fract(line_lvl_mod);
  }
  frag_level = temp; //select(1.f, temp, line_lvl < (grid_buf.num_levels - 1));
  line_lvl = line_lvl + int(ceil(line_lvl_mod));// start grid at 100 subdivs
  
  // Discard sublevels to match old grid visually
  if (line_lvl < 2) {
    gl_Position = float4(NAN_FLT);
    return;
  }
  
  // Offset levels by specified amount so as to always render a sublevel
  line_lvl += GRID_LEVEL_OFFSET;

  // Determine line scale for current level; 
  // use float to support fractional scaling for sub-levels
  float line_scale = pow(float(GRID_SUBDIVS), float(line_lvl));

  // Offset distance to most outer line
  float line_start = -line_scale * float(grid_buf.num_lines >> 1);

  // Determine vertex position
  // If not start vertex, flip y-coord for other side of line
  // If not x-direction, flip x- and -ycoords for y-direction
  float3 vert_pos = float3((line_start + line_scale * line_idx), line_start, 0.0f);
  vert_pos.y = select(vert_pos.y, -vert_pos.y, line_side);
  vert_pos.xy = select(vert_pos.xy, vert_pos.yx, line_dir);

  // Output vertex position in [-1, 1]
  frag_xy = vert_pos.xy / line_start;

  // Add camera offset; grid moves with camera
  float3 pos_on_floor = float3(drw_view_position().xy + t * -drw_view_forward().xy, 0);
  vert_pos += round(pos_on_floor / (line_scale * GRID_SUBDIVS)) * (line_scale * GRID_SUBDIVS);
  
  // Vertex outputs
  local_pos = vert_pos;

  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vert_pos, 1.0f));
}