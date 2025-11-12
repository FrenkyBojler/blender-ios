/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

/** Keep in sync with `SI_GRID_STEPS_LEN` in `DNA_space_types.h`. */
#define GRID_LEVELS_TOTAL 8
/** The grid renders N hardcoded levels of hierarchy. */
#define GRID_LEVELS_DRAW 3

/* Helper struct: a vertex as part of a line is defined by its 2D position, 
 * and the grid level it belongs on. See `get_line_data()` below. */
struct LineData {
  float2 P;
  int level;
};

LineData get_line_data(in uint vertex_id)
{
  LineData line;

  /* Every pair of consecutive vertices forms a line, indicated by bit 0. */
  uint side = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;

  /* Every pair of consecutive lines alternates x/y direction, indicated by bit 1. */
  uint dir = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;

  /* The index/level of a line are implicitly encoded by the 30 remaining bits. */
  line.level = int(vertex_id / grid_buf.num_lines_per_level);
  vertex_id = vertex_id % grid_buf.num_lines_per_level;

  /* From the index, generate 2*N+1 points equidistantly spaced on [-N/2, N/2]. */
  line.P.x = float(grid_buf.num_lines_per_level >> 1u);
  line.P.y = float(vertex_id) - line.P.x;

  /* If this isn't the start of the line, flip the x-coord to define the end. */
  line.P.x = select(line.P.x, -line.P.x, side);

  /* If this isn't the x-direction, flip x/y-coords to define the y-direction. */
  line.P.xy = select(line.P.xy, line.P.yx, dir);

  return line;
}

void main()
{
  LineData line = get_line_data(gl_VertexID);

  /* Set several fragment stage outputs fed into the alpha component:
   * - The vertex position as [-1, 1], so we can fade level boundaries.
   * - The fade for the *lowest* level, fitted to a quadratic curve. */
  frag_xy = line.P / float(grid_buf.num_lines_per_level >> 1);
  frag_level = line.level > 0 ? 1.0f : 1.0f - square(fract(grid_level));

  // if (line.level == 1 /* > 0 && line.level < GRID_LEVELS_DRAW - 1 */) {
  //   if 
  //   gl_Position = float4(NAN_FLT);
  //   return;
  // }

  /* Compute the actual level of grid data, offset negatively to always show one sub-level */
  line.level = int(grid_level) + line.level - 1;
  line.level = clamp(line.level, 0, GRID_LEVELS_TOTAL - 1);


  /* Dependent on flags, we now put the grid on the correct plane. */
  float3 P;
  float3 P_offset;
  if (flag_test(grid_flag, PLANE_XY)) {
    P = float3(line.P.x, line.P.y, 0.0f);
    P_offset = float3(grid_poi.x, grid_poi.y, 0.0f);
  }
  else if (flag_test(grid_flag, PLANE_XZ)) {
    P = float3(line.P.x, 0.0f, line.P.y);
    P_offset = float3(grid_poi.x, 0.0f, grid_poi.y);
  }
  else if (flag_test(grid_flag, PLANE_YZ)) {
    P = float3(0.0f, line.P.x, line.P.y);
    P_offset = float3(0.0f, grid_poi.x, grid_poi.y);
  }
  else { /* PLANE_IMAGE */
    P = float3(line.P.xy * 0.5f + 0.5f, 0.0f);
  }

  /* Scale the grid based on level. Additionally, translate the grid with the point of interest,
   * in increments dependent on the level's scaling. */
  float scale = grid_buf.level_scales[line.level].x;
  P = (P + round(P_offset / scale)) * scale;

  /* For the in-between levels, we discard lines that overlap with the higher-up levels. */
  /* If there exists an integer, such that with the scaling of the level above we can draw
   * the current line, we can safely clip the current line. */
  /* if (line.level < GRID_LEVELS_DRAW - 1) {
    float2 nearest = P.xy / grid_buf.level_scales[line.level + 1].x;
    if (any(equal(nearest - round(nearest), float2(0)))) {
      gl_Position = float4(NAN_FLT);
      return;
    }
  } */

  /* Output the world-space position to the fragment stage, and cull minute scales; they are
   * hard to draw without a much larger amount of geometry. This mirrors old grid behavior. */
  local_pos = P;
  if (scale <= 1e-3f) {
    gl_Position = float4(NAN_FLT);
  }
  else {
    gl_Position = drw_view().winmat * (drw_view().viewmat * float4(P, 1.0f));
  }
}
