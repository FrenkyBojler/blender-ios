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
#define GRID_LEVELS_DRAW 2

/* Helper function; discard the current line vertex in top scope. */
#define discard_line() { gl_Position = float4(NAN_FLT); return; }

/* Helper struct: a vertex as part of a line is defined by its 2D position,
 * and the grid level it belongs on. See `get_line_data()` below. */
struct LineData {
  uint dir;
  uint side;
  float2 P;
  int level;
};

LineData get_line_data(in uint vertex_id)
{
  LineData line;

  /* Every pair of consecutive vertices forms a line, indicated by bit 0. */
  line.side = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;

  /* Every pair of consecutive lines alternates x/y direction, indicated by bit 1. */
  line.dir = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;

  /* The index/level of a line are encoded by the 30 remaining bits. */
  line.level = int(vertex_id / grid_buf.num_lines_per_level);
  vertex_id = vertex_id % grid_buf.num_lines_per_level;

  /* From the index, generate N+1 points equidistantly spaced on [-N/2, N/2]. */
  line.P.x = float(grid_buf.num_lines_per_level >> 1u); /* N/2 */
  line.P.y = float(vertex_id) - line.P.x;               /* [0...N] - N/2 */

  /* If this isn't the start of the line, flip the x-coord to define the end. */
  line.P.x = select(line.P.x, -line.P.x, line.side);

  /* If this isn't the x-direction, flip x/y-coords to define the y-direction. */
  line.P.xy = select(line.P.xy, line.P.yx, line.dir);

  return line;
}

void main()
{
  LineData line = get_line_data(gl_VertexID);

  // TODO remove
  debug_level = line.level;

  /* Set several fragment stage outputs:
   * - Vertex position [-1, 1], so we can fade level boundaries.
   * - The fade for the *lowest* level, fitted to a curve. */
  local_coord = line.P / float(grid_buf.num_lines_per_level >> 1);
  local_alpha = line.level > 0 ? 1.0f : 1.0f - fract(grid_level);

  /* Compute the actual level of grid data, offset by -1 to always draw a sub-level. */
  int level = int(grid_level) + line.level - 1;

  /* We clip a line if the level range is outside the specified unit system's data. */
  if (level < 0 || level >= GRID_LEVELS_TOTAL) {
    discard_line();
  }

  /* Configure values on XY plane first. */
  float2 P = line.P.xy;
  float2 P_offset;
  if (flag_test(grid_flag, PLANE_XY)) {
    P_offset = grid_poi.xy;
  }
  else if (flag_test(grid_flag, PLANE_XZ)) {
    P_offset = grid_poi.xz;
  }
  else if (flag_test(grid_flag, PLANE_YZ)) {
    P_offset = grid_poi.yz;
  }
  else { /* PLANE_IMAGE */ /* TODO (not_mark): test for PLANE_IMAGE */
    P_offset = grid_poi.xy;
  }

  /* Scale the grid based on level. Additionally, translate the grid with the point of interest,
   * in increments dependent on the level's scaling. */
  float scale = grid_buf.level_scales[level].x;
  P *= scale;

  /* Modify fade based on pixel size for orthographic, as we lack proper dfdx/dfdy on lines. */
  if (!drw_view_is_perspective()) {
    float fade = smoothstep(scale * 0.25, scale * pow3f(0.25), uniform_buf.pixel_fac);
    local_alpha *= fade;
  }

  /* We do manual clipping/clamping to a reasonable range for float precision, as (absurdly) large
   * lines can cause flickering/teleporting problems. */
  float2 clip = drw_view_is_perspective()
              ? float2(grid_buf.distance)
              : float2(max(8.0 / drw_view().winmat[0][0], 8.0 / drw_view().winmat[1][1]));
  if (all(greaterThan(abs(P), clip))) {
    discard_line(); /* Both x, y lie outside the clip distance. */
  } else {
    P = clamp(P, -clip, clip);
  }

  /* Add scaled camera offset. */
  P += round(P_offset / scale) * scale;

  /* If there exists an integer, such that with the scaling of the level *above* we can draw
   * the current line, we can discard the current line on *any* sublevel as the superlevel
   * is guaranteed to draw over it. */
  if (line.level < GRID_LEVELS_DRAW - 1 && level < GRID_LEVELS_TOTAL - 1) {
    float nscale = grid_buf.level_scales[min(level + 1, GRID_LEVELS_TOTAL - 1)][0];
    float offset = round(select(P_offset.y, P_offset.x, line.dir) / nscale) * nscale;
    float P_diff = offset + (select(P.y, P.x, line.dir) - offset) / nscale;
    /* Accounting for rounding here. */
    if (abs(fract(P_diff)) < 1e-4) {
      discard_line();
    }
  }

  /* Output the world-space position to the fragment stage.*/
  if (flag_test(grid_flag, PLANE_XY)) {
    local_pos = float3(P.x, P.y, 0.0f);
  }
  else if (flag_test(grid_flag, PLANE_XZ)) {
    local_pos = float3(P.x, 0.0f, P.y);
  }
  else if (flag_test(grid_flag, PLANE_YZ)) {
    local_pos = float3(0.0f, P.x, P.y);
  }
  else { /* PLANE_IMAGE */
    local_pos = float3(line.P.xy * 0.5f + 0.5f, 0.0f);
  }
  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(local_pos, 1.0f));
}
