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

/* Helper function; discard the current line vertex in top scope. */
#define discard_line() \
  { \
    gl_Position = float4(NAN_FLT); \
    return; \
  }

/* Helper struct: a vertex as part of a line is defined by a begin/end vertex position,
 * and the level it is placed on. See `get_line_data()` below. */
struct LineData {
  float2 P;
  uint dir;
  int level;
};

LineData get_line_data(in uint vertex_id)
{
  LineData line;

  /* Every pair of consecutive vertices forms a line, indicated by bit 0.
   * Every pair of consecutive lines alternates x/y direction, indicated by bit 1. */
  uint side = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;
  line.dir = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;

  /* The index/level of a line are encoded by the 30 remaining bits. */
  line.level = int(vertex_id / grid_buf.num_lines_per_level);
  vertex_id = vertex_id % grid_buf.num_lines_per_level;

  /* From the index, generate N+1 points equidistantly spaced on [-N/2, N/2]. */
  line.P.x = float(grid_buf.num_lines_per_level >> 1u); /* N/2 */
  line.P.y = float(vertex_id) - line.P.x;               /* [0...N] - N/2 */

  /* If this isn't the start of the line, flip the x-coord to define the end. */
  line.P.x = select(line.P.x, -line.P.x, side);

  /* If this isn't the x-direction, flip x/y-coords to define the y-direction. */
  line.P.xy = select(line.P.xy, line.P.yx, line.dir);

  return line;
}

void main()
{
  LineData line = get_line_data(gl_VertexID);

  /* Set several fragment stage outputs:
   * - Vertex position [-1, 1], so we can fade level boundaries.
   * - The fade for the *lowest* level, as a linear line. */
  local_coord = line.P / float(grid_buf.num_lines_per_level >> 1);
  local_alpha
    = ((line.level + 1.0f - fract(grid_level)) / float(GRID_LEVELS_DRAW));
  local_alpha = 2.0f * local_alpha - square(local_alpha);

  /* All values operate on the X, Y plane for simplicity. */
  float2 P = line.P;
  float2 P_offset = grid_poi;

  /* Compute the actual level of grid data, offset by -1 to draw a sub-level. Then
   * scale the grid line based on this level */
  int level = int(grid_level) + line.level;
  if (!flag_test(grid_flag, PLANE_IMAGE)) {
    level -= 1;
  }
  float scale = grid_buf.level_scales[level][line.dir];
  P *= scale;

  /* Modify fade based on pixel size for orthographic, as we lack proper dfdx/dfdy on lines. */
  if (!drw_view_is_perspective()) {
    float fade = smoothstep(scale * 0.25f, scale * pow3f(0.25f), uniform_buf.pixel_fac);
    if (gl_VertexID == 0) 
      printf("fade: %f, scale: %f, fac: %f\n", fade, scale, uniform_buf.pixel_fac);
    local_alpha *= fade;
  }

  debug_level = line.level;

  /* Clipping; discard lines outside of the level range. */
  if (level < 0 || level >= GRID_LEVELS_TOTAL) {
    discard_line();
  }

  /* Clipping; restrict lines to a reasonable range for float precision, as (absurdly) large
   * lines can cause flickering/teleporting problems. */
  /* TODO (not_mark): re-enable */
  // float2 clip;
  // if (drw_view_is_perspective()) {
  //   clip = float2(grid_buf.distance);
  // } else if (!flag_test(grid_flag, PLANE_IMAGE)) { /* Orthographic camera */
  //   clip = float2(max(8.0 / drw_view().winmat[0][0], 8.0 / drw_view().winmat[1][1]));
  // } else { /* PLANE_IMAGE */
  //   clip = grid_buf.size.xy;
  // }
  float2 clip = drw_view_is_perspective() ?
                    float2(grid_buf.distance) :
                    float2(max(8.0 / drw_view().winmat[0][0], 8.0 / drw_view().winmat[1][1]));
  // if (all(greaterThan(abs(P), clip))) {
  //   discard_line(); /* Both x, y lie outside the clip distance. */
  // } else {
  //   P = clamp(P, -clip, clip);
  // }

  /* Add scaled camera offset, rounded to the nearest level-dependent line position. */
  P += round(P_offset / scale) * scale;

  /* Clipping; restrict the grid in the UV/Image editor to the specified tile sizes */
  if (flag_test(grid_flag, PLANE_IMAGE)) {
    P = clamp(P, float2(-1.0f), 2.0f * grid_buf.size.xy - 1.0f);
  }

  /* Clipping; if there exists an integer, s.t. with the scaling of the level *above* we can draw
   * the current line, we can discard the current line on *any* sublevel as the superlevel
   * is guaranteed to draw over it. */
  /* TODO (not_mark): fix the weird cases during [orthographic+imperial+`thou`] and re-enable. */
  /* if (line.level < GRID_LEVELS_DRAW - 1 && level < GRID_LEVELS_TOTAL - 1) {
    float nscale = grid_buf.level_scales[min(level + 1, GRID_LEVELS_TOTAL - 1)][0];
    float offset = round(select(P_offset.y, P_offset.x, line.dir) / nscale) * nscale;
    float P_diff = offset + (select(P.y, P.x, line.dir) - offset) / nscale;
    if (abs(fract(P_diff)) < 1e-4) {
      discard_line();
    }
  } */

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
    local_pos = float3(P.xy * 0.5f + 0.5f, 0.0f);
  }
  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(local_pos, 1.0f));
}
