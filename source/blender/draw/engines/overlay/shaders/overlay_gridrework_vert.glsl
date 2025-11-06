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

// TODO delete when done
/* uint getLineLvlOffset(uint i)
{
  uint j = 0x55555555 >> (0xFFFFFFFE & (30 - findMSB(i)));
  return i < j ? j >> 2 : j;
} */

void main()
{
  uint line_idx = gl_VertexID;
  
  // Every pair of consecutive vertices forms a line; indicated by bit 0 of gl_VertexID
  uint line_side = line_idx & 0x1;
  line_idx = line_idx >> 1;

  // Every pair of consecutive lines alternates x/y direction, indicated by bit 1 of gl_VertexID
  uint line_dir = line_idx & 0x1;
  line_idx = line_idx >> 1;

  // Lines are repeated for every level, so divide gives us the line's level ...
  uint line_lvl = int((idx / grid_buf.num_lines) % grid_buf.num_levels);
  // ... while modulo gives us the line index on said level
  line_idx = line_idx % grid_buf.num_lines;

  // Discard lines that are present in a level above
  /* if (abs(int(line_idx) - int(grid_buf.num_lines / 2)) % GRID_SUBDIVS == 0 && line_lvl < (GRID_LEVELS - 1)) {
    gl_Position = float4(NAN_FLT);
    return;
  } */

  // Determine distance to floor plane through center point
  float abs_cos_theta = abs(drw_view_forward().z); 
  float abs_z = abs(drw_view_position().z);
  float t = abs_z / abs_cos_theta;
  t = mix(t, abs_z, 1.0f - abs_cos_theta);
  
  // We next determine the actual vertex position forming the line ...
  // ... use float to support fractional scaling at sub-levels
  float line_start_y = 0.f;
  // float line_start_x = 

  float line_start_y = -max(float(grid_buf.num_lines >> 1), 1.f);
  float line_start_x = lvl != 0 ? line_start_y + line_idx : 0.f;
  float3 line_pos = float3(line_start_x, line_start_y, 0.0f);
  // If not start vertex, flip y-coord to define end vertex
  line_pos.y = select(line_pos.y, -line_pos.y, line_side);
  // If not x-direction, flip x/y-coords for y-direction
  line_pos.xy = select(line_pos.xy, line_pos.yx, line_dir);



  // Discard sublevels to match old grid visually
  /* if (line_lvl < 2) {
    gl_Position = float4(NAN_FLT);
    return;
  } */

  // Skip middle, this is handled by the uppermost layer
  /* if (lvl != 0 && line_idx >= lvl_size / 2) {
    line_idx++;
  } */

  // Offset levels by specified amount so as to always render a sublevel
  // lvl += GRID_LEVEL_OFFSET;


  // TODO remove when debug is no longer necessary
  debug_grid_lvl = lvl;

  // Output vertex position in [-1, 1]
  frag_xy = line_pos.xy / line_start_y;
  
  // Value to offset levels by, dependent on distance to center point
  float lvl_mod = /* max(0.f, */ log2(t) / log2(float(GRID_SUBDIVS)); // log10(t

  // Rotate levels dependent on distance to floor plane point)
  // Grid levels fade in smoothly, passed through a sigmoidal
  float temp = 1.f;
  if (lvl == 0) {
    temp = 1.f - fract(lvl_mod);
  } else {
    temp = 1.0f - (float(lvl) + fract(lvl_mod)) / float(grid_buf.num_levels - 1);
  }
  // if (lvl < (grid_buf.num_levels - 1)) {
  // } else if (lvl == (grid_buf.num_levels - 1)) {
  //   temp = fract(lvl_mod);
  // }
  frag_level = temp; //select(1.f, temp, line_lvl < (grid_buf.num_levels - 1));
  lvl -= int(ceil(lvl_mod));// start grid at 100 subdivs

  // From here on we invert levels. The bottom of the quadtree forms level 0, and so on.
  // This makes the stuff below a little easier
  int max_lvl = int(grid_buf.num_levels) - 1 + GRID_LEVEL_OFFSET;
  lvl = max_lvl - lvl;

  // Scale by level subdivision
  float line_scale = pow(float(GRID_SUBDIVS), float(lvl));
  line_pos.xy *= line_scale;
  // line_pos.xy = sign(line_pos.xy) * min(abs(line_pos.xy) * line_scale, float2(grid_buf.distance * 0.5f));
  
  // Add camera offset; grid moves with camera
  float3 pos_on_floor = float3(drw_view_position().xy + t * -drw_view_forward().xy, 0);
  // line_pos += round(pos_on_floor / (line_scale * GRID_SUBDIVS)) * (line_scale * GRID_SUBDIVS);
  line_pos += round(pos_on_floor / (line_scale)) * (line_scale);
  
  // Vertex outputs
  local_pos = line_pos;

  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(line_pos, 1.0f));
}