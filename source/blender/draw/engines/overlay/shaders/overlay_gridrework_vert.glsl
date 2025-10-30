/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

// TODO merge with vert definition
#define NUM_GRID_LEVELS     3
#define NUM_GRID_DIRECTIONS 2
#define NUM_GRID_SUBDIVS    10

void main()
{
  // Extract line data from vertex id
  uint idx = gl_VertexID;
  uint line_side = idx & 0x01;                                                            // LSB indicates start/end vertex
  idx = idx >> 1;
  uint line_idx = (idx % grid_buf.num_lines);                                             // index of line, from x-min to x-max
  uint line_dir = (idx / grid_buf.num_lines) % NUM_GRID_DIRECTIONS;                       // direction of line, x or y
  int line_lvl = int((idx / grid_buf.num_lines / NUM_GRID_DIRECTIONS) % NUM_GRID_LEVELS); // lvl of line on grid, wraps around

  // Determine distance to floor plane through center point
  float abs_cos_theta = abs(drw_view_forward().z); 
  float abs_z = abs(drw_view_position().z);
  float t = abs_z / abs_cos_theta;
  t = mix(t, abs_z, 1.0f - abs_cos_theta);

  // Rotate levels dependent on distance to floor plane point
  float line_lvl_mod = log2(t) / log2(float(NUM_GRID_SUBDIVS)); // log10(t) equals log2(t) / log2(10)
  
  // Output level interpolant for blending in fragment shader
  local_level = (float(line_lvl - 1) + line_lvl_mod) / float(NUM_GRID_LEVELS - 1);
  if (local_level > 0)
    local_level = fract(local_level);
  else
    local_level = 1.f - fract(local_level);


  // local_level 
  //   = mod(float(line_lvl /* - 1 */) + line_lvl_mod, float(NUM_GRID_LEVELS - 1)) 
  //   / float(NUM_GRID_LEVELS - 1);

  line_lvl += int(line_lvl_mod);
  // line_lvl -= 1; // Start grid one level below current


  if (gl_VertexID == 0)
    printf("t = %f, mod = %f, actual level = %d, local level = %f\n", t, line_lvl_mod, line_lvl, local_level);


  // Determine line scale for current level; 
  // use float to support fractional scaling for sub-levels
  float line_scale = pow(float(NUM_GRID_SUBDIVS), float(line_lvl));

  // Offset distance to most outer line
  float line_start = -line_scale * float(grid_buf.num_lines >> 1);

  // Determine vertex position
  // If not start vertex, flip y-coord for other side of line
  // If not x-direction, flip x- and -ycoords for y-direction
  float3 vert_pos = float3((line_start + line_scale * line_idx), line_start, 0.0f);
  vert_pos.y = select(vert_pos.y, -vert_pos.y, line_side);
  vert_pos.xy = select(vert_pos.xy, vert_pos.yx, line_dir);

  // Add camera offset; grid moves with camera
  float3 pos_on_floor = float3(drw_view_position().xy + t * -drw_view_forward().xy, 0);
  // pos_on_floor = mix(pos_on_floor, float3(drw_view_position().xy, 0), 0.f);
  // pos_on_floor = 0.5f * pos_on_floor + 0.5f * float3(drw_view_position().xy, 0);
  vert_pos += round(pos_on_floor / (line_scale * NUM_GRID_SUBDIVS)) * (line_scale * NUM_GRID_SUBDIVS);

  // Per vertex, output mixture value in [0, NUM_GRID_LEVELS]
  // local_level = float(line_lvl) / float(NUM_GRID_LEVELS);

  // Vertex outputs
  local_pos = vert_pos;
  local_coord = float(line_side);

  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vert_pos, 1.0f));
}