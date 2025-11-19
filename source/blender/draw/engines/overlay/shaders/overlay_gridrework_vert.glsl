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

/* Helper function; discard the current line vertex in top scope; it will be clipped. */
#define discard_line() \
  { \
    gl_Position = float4(NAN_FLT); \
    return; \
  }

struct LineData {
  float2 P;
  uint dir;
  int level;
};

/* Helper; gl_VertexID implicitly encodes an axis line; this is only used for the
 * positive/negative z axis when the rest of the grid is drawn on the xy plane.  */
LineData decode_zaxis_data(in uint vertex_id) {
  LineData line;

  /* Every pair of consecutive vertices forms a line, indicated by bit 0. */
  uint side = vertex_id & 0x1u;
  /* From side/dir, generate a line along [-1,0] or [0,1] dependent on grid_flag. */
  line.P.x = 0.0f;
  line.P.y = float(grid_buf.num_lines_per_level >> 1u)
    * select(0.0f, select(-1.0f, 1.0f, flag_test(grid_flag, CLIP_ZPOS)), side);
  /* Always top-drawn level. */
  line.level = GRID_LEVELS_DRAW - 1;

  return line;
}

/* Helper; gl_VertexID implicitly encodes a grid line; this is used for drawing the entire
 * grid plus two axes on the same plane plane. */
LineData decode_grid_data(in uint vertex_id)
{
  LineData line;

  /* Every pair of consecutive vertices forms a line, indicated by bit 0.
   * Every pair of consecutive lines alternates x/y, indicated by bit 1. */
  uint side = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;
  line.dir = vertex_id & 0x1u; /* Stored for later lookup. */
  vertex_id = vertex_id >> 1u;

  /* The index/level of a line are encoded by the 30 remaining bits. */
  line.level = int(vertex_id / grid_buf.num_lines_per_level);
  vertex_id = vertex_id % grid_buf.num_lines_per_level;

  /* From the index, generate N+1 points equidistantly spaced on [-N/2, N/2]. */
  line.P.x = float(grid_buf.num_lines_per_level >> 1u); /* N/2 */
  line.P.y = float(vertex_id) - line.P.x;               /* [0...N] - N/2 */

  /* If this isn't the start of the line, flip the x-component to the end. Likewise,
   * if this isn't the x-direction, flip components to define the y-direction. */
  line.P.x = select(line.P.x, -line.P.x, side);
  line.P.xy = select(line.P.xy, line.P.yx, line.dir);

  return line;
}

void main()
{
  LineData line;
  if (flag_test(grid_flag, CLIP_ZPOS) || flag_test(grid_flag, CLIP_ZNEG)) {
    line = decode_zaxis_data(gl_VertexID);
  } else {
    line = decode_grid_data(gl_VertexID);
  }

  /* Compute the actual level of grid data, offset by -1 to force a sub-level in the 3D viewport. */
  int level = int(grid_level) + line.level - (flag_test(grid_flag, PLANE_IMAGE) ? 0 : 1);
  float scale = grid_buf.level_scales[level][line.dir];

  /* Clipping; discard lines outside of the level range. */
  if (level < 0 || level >= GRID_LEVELS_TOTAL) {
    discard_line();
  }

  /* Stage output: vertex position in [-1,1], which we use to fade level boundaries. */
  local_coord = line.P / float(grid_buf.num_lines_per_level >> 1);

  /* Stage output:  level fade in [0, 1], which we use to smoothly transition grid levels. */
  local_alpha = (line.level + 1.0f - fract(grid_level)) / float(GRID_LEVELS_DRAW - 1);
  local_alpha = saturate(local_alpha);
  local_alpha = 2.0f * local_alpha - square(local_alpha); /* Slight elliptic curve. */
  /* Modify fade based on pixel size for orthographic, as we lack proper dfdx/dfdy on lines. */
  if (!drw_view_is_perspective()) {
    float fade = smoothstep(scale * 0.25f, scale * pow3f(0.25f), uniform_buf.pixel_fac);
    local_alpha *= fade;
  }

  /* Scale lines appropriately based on level scaling. */
  line.P *= scale;

  /* Clipping; restrict lines to a reasonable range for float precision, as (absurdly) large
   * lines can cause flickering/teleporting problems. */
  /* TODO (not_mark): fix in UV/Image editor or combine with clipping below */
  float2 clip = drw_view_is_perspective() 
    ? float2(grid_buf.distance) 
    : float2(8.0 / max(drw_view().winmat[0][0], drw_view().winmat[1][1]));
  if (all(greaterThan(abs(line.P), clip))) {
    discard_line(); /* Both x, y lie outside the clip distance. */
  } else {
    line.P = clamp(line.P, -clip, clip);
  }

  /* Add scaled camera offset, rounded to the nearest level-dependent line position. */
  line.P += round(grid_poi / scale) * scale;

  /* Clipping; restrict the grid in the UV/Image editor to the specified tile sizes */
  if (flag_test(grid_flag, PLANE_IMAGE)) {
    line.P = clamp(line.P, float2(-1.0f), 2.0f * grid_buf.size.xy - 1.0f);
  }

  /* Clipping; if there exists an integer, s.t. with the scaling of the level *above* we can draw
   * the current line, we can discard the current line on *any* sublevel as the superlevel
   * is guaranteed to draw over it. */
  /* TODO(not_mark): disabled until I can identify popping issues. */
  /* if (!flag_test(grid_flag, PLANE_IMAGE)) {
    if (line.level < GRID_LEVELS_DRAW - 1 && level < GRID_LEVELS_TOTAL - 1) {
      float nscale = grid_buf.level_scales[min(level + 1, GRID_LEVELS_TOTAL - 1)][0];
      float offset = round(select(grid_poi.y, grid_poi.x, line.dir) / nscale) * nscale;
      float P_diff = offset + (select(line.P.y, line.P.x, line.dir) - offset) / nscale;
      if (abs(fract(P_diff)) < 1e-4) {
        discard_line();
      }
    }
  } */

  /* Output the world-space position on the correct plane dependent on camera settings. */
  if (flag_test(grid_flag, PLANE_XY)) {
    local_pos = float3(line.P.x, line.P.y, 0.0f);
  }
  else if (flag_test(grid_flag, PLANE_XZ)) {
    local_pos = float3(line.P.x, 0.0f, line.P.y);
  }
  else if (flag_test(grid_flag, PLANE_YZ)) {
    local_pos = float3(0.0f, line.P.x, line.P.y);
  }
  else { /* PLANE_IMAGE */
    local_pos = float3(line.P.xy * 0.5f + 0.5f, 0.0f);
  }
  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(local_pos, 1.0f));

  /* Output for viewport antialiasing. */
  edge_start = edge_pos 
    = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) * uniform_buf.size_viewport;
}
