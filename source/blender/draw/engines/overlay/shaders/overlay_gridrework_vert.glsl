/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"

// TODO merge with vert definition
#define GRID_SUBDIVS    10
#define GRID_LEVEL_BASE -2

void main()
{
  uint line_idx = gl_VertexID;
  
  /* Every pair of consecutive vertices forms a line, indicated by bit 0 of gl_VertexID. */
  uint line_side = line_idx & 0x1;
  line_idx = line_idx >> 1;

  /* Every pair of consecutive lines alternates x/y direction, indicated by bit 1 of gl_VertexID. */
  uint line_dir = line_idx & 0x1;
  line_idx = line_idx >> 1;

  /* Lines are repeated `grid_buf.num_lines` times for every level, so a divide 
   * by `grid_buf.num_lines` provides the line's level... */
  int line_lvl = int((line_idx / grid_buf.num_lines) % grid_buf.num_levels);
  /* ...while the remainder provides the index of the line on said level */
  line_idx = line_idx % grid_buf.num_lines;

  /* If a line is present in a level above, we can discard it.
  /* NOTE: this is disabled as it is not the case when the grid moves after the camera */
  /* int offs = abs(int(grid_buf.num_lines / 2) - int(line_idx));
  if (offs % GRID_SUBDIVS == 0 && line_lvl < grid_buf.num_levels - 1) {
    gl_Position = float4(NAN_FLT);
    return;
  } */
  
  // We next determine the actual vertex position forming the line ...
  // ... use float to support fractional scaling at sub-levels
  float line_start_y = -max(float(grid_buf.num_lines >> 1), 1.f);
  float line_start_x = line_start_y + float(line_idx);
  float3 vert_pos = float3(line_start_x, line_start_y, 0.0f);
  // If not start vertex, flip y-coord to define end vertex instead
  vert_pos.y = select(vert_pos.y, -vert_pos.y, line_side);
  // If not x-direction, flip coordinates to define y-direction instead
  vert_pos.xy = select(vert_pos.xy, vert_pos.yx, line_dir);

  /* Output the vertex position in [-1, 1] to fragment state for smooth fade. */
  frag_xy = vert_pos.xy / line_start_y;
  
  // We now adjust the grid level dependent on distance to a point on the floor plane
  // First; determine distance to this point
  float abs_cos_theta = abs(drw_view_forward().z); 
  float abs_z = abs(drw_view_position().z);
  float t = mix(abs_z / abs_cos_theta, abs_z, 1.0f - abs_cos_theta);
  // Then; determine a fractional level offset
  float line_lvl_offset = log2(t) / log2(float(GRID_SUBDIVS)); /* log10(t) = log2(t) / log2(10) */
  // Next; to fade grid levels in/out smoothly, output a fade factor to fragment
  if (line_lvl < (grid_buf.num_levels - 1)) {
    frag_level = (float(line_lvl) + 1.f - fract(line_lvl_offset)) / float(grid_buf.num_levels - 1);
  } else {
    frag_level = fract(line_lvl_offset);
  }
  // Finally; adjust line level by the nearest upper component and a base offset
  line_lvl = GRID_LEVEL_BASE + line_lvl + int(ceil(line_lvl_offset));

  // TODO remove when debug is no longer necessary
  debug_grid_lvl = line_lvl;

  /* Each level of the grid is an order of magnitude larger than the previous level */
  float line_scale = pow(float(GRID_SUBDIVS), float(line_lvl));
  vert_pos *= line_scale;

  /* The grid moves with the camera in increments so as to go unnoticed. */
  float3 pos_on_floor = float3(drw_view_position().xy + t * -drw_view_forward().xy, 0);
  vert_pos += round(pos_on_floor / line_scale) * (line_scale);

  // Discard sublevels to match old grid visually
  /* if (line_lvl < 2) {
    gl_Position = float4(NAN_FLT);
    return;
  } */

  // Skip middle, this is handled by the uppermost layer
  /* if (lvl != 0 && line_idx >= lvl_size / 2) {
    line_idx++;
  } */

  // Value to offset levels by, dependent on distance to center point
  // float lvl_mod = /* max(0.f, */ log2(t) / log2(float(GRID_SUBDIVS)); // log10(t

  // Rotate levels dependent on distance to floor plane point)
  // Grid levels fade in smoothly, passed through a sigmoidal
  // float temp = 1.f;
  // if (lvl == 0) {
  //   temp = 1.f - fract(lvl_mod);
  // } else {
  //   temp = 1.0f - (float(lvl) + fract(lvl_mod)) / float(grid_buf.num_levels - 1);
  // }
  // if (lvl < (grid_buf.num_levels - 1)) {
  // } else if (lvl == (grid_buf.num_levels - 1)) {
  //   temp = fract(lvl_mod);
  // }
  // frag_level = temp; //select(1.f, temp, line_lvl < (grid_buf.num_levels - 1));
  // lvl -= int(ceil(lvl_mod));// start grid at 100 subdivs

  // From here on we invert levels. The bottom of the quadtree forms level 0, and so on.
  // This makes the stuff below a little easier
  // int max_lvl = int(grid_buf.num_levels) - 1 + GRID_LEVEL_BASE;
  // lvl = max_lvl - lvl;

  // Scale by level subdivision
  // float line_scale = pow(float(GRID_SUBDIVS), float(lvl));
  // vert_pos.xy *= line_scale;
  // vert_pos.xy = sign(vert_pos.xy) * min(abs(vert_pos.xy) * line_scale, float2(grid_buf.distance * 0.5f));
  
  // Vertex outputs
  local_pos = vert_pos;

  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vert_pos, 1.0f));
}