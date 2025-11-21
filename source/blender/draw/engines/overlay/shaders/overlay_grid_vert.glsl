/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_grid_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

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

/* Helper; gl_VertexID implicitly encodes a grid line. */
LineData decode_grid_data(in uint vertex_id)
{
  LineData line;

  /* Every pair of consecutive verts forms a line, indicated by bit 0.
   * Every pair of consecutive lines flips x/y, indicated by bit 1. */
  uint side = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;
  line.dir = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;

  /* The index/level of a line are encoded by the 30 remaining bits. Note that we order
   * levels from "large" to "small", prioritizing output of the larger levels. */
  line.level = OVERLAY_GRID_STEPS_DRAW - 1 - int(vertex_id / grid_buf.num_lines);
  vertex_id = vertex_id % grid_buf.num_lines;

  /* From the index, generate N+1 points equidistantly spaced on [-N/2, N/2]. */
  line.P.x = max(float(grid_buf.num_lines >> 1u), 1.0f);
  line.P.y = (float(vertex_id) - float(grid_buf.num_lines >> 1u));

  /* If this isn't the start of the line, flip the x-component to the end. Likewise,
   * if this isn't the x-direction, flip components to define the y-direction. */
  line.P.x = select(line.P.x, -line.P.x, side);
  line.P.xy = select(line.P.xy, line.P.yx, line.dir);

  return line;
}

/* Helper; gl_VertexID implicitly encodes one of three axis lines. */
LineData decode_axis_data(in uint vertex_id)
{
  LineData line;

  /* Every pair of consecutive verts forms a line, indicated by bit 0.
   * They then alternate x/y/z, indicated by the other 31 bits. */
  uint side = vertex_id & 0x1u;
  line.dir = vertex_id >> 1u;
  /* For an axis line, the level is fixed, and the direction is simply the vertex index. */
  line.level = OVERLAY_GRID_STEPS_DRAW - 1;
  /* Output a vertex as [-N/2, N/2], [0, 0]. */
  line.P.x = max(float(grid_buf.num_lines >> 1u), 1.0f) * select(1.0f, -1.0f, side);
  line.P.y = 0.0f;

  return line;
}

void main()
{
  LineData line;
  if (flag_test(grid_flag, SHOW_GRID)) {
    line = decode_grid_data(gl_VertexID);
  }
  else if (flag_test(grid_flag, SHOW_AXES)) {
    line = decode_axis_data(gl_VertexID);
  }

  /* Compute the actual level of a line, offset by -1 to force a sublevel in the 3D viewport. */
  int level = int(grid_level) + line.level - (flag_test(grid_flag, PLANE_IMAGE) ? 0 : 1);
  if (level < 0 || level >= OVERLAY_GRID_STEPS_LEN) {
    discard_line();
  }

  float scale = grid_buf.level_scales[level][line.dir];

  /* Stage outputs. */
  {
    /* Stage output: vertex position in [-1,1], which we use to fade level boundaries. */
    local_coord = line.P / max(float(grid_buf.num_lines >> 1), 1.0f);

    /* Stage output: level fade in [0, 1], which we use to smoothly transition grid levels. */
    local_alpha = (line.level + 1.0f - fract(grid_level)) / float(OVERLAY_GRID_STEPS_DRAW - 1);
    local_alpha = saturate(local_alpha);
    local_alpha = 2.0f * local_alpha - square(local_alpha); /* Upside down parabola curve. */

    /* Fade by pixel size for orthographic, as we lack proper line dfdx/dfdy. */
    if (!drw_view_is_perspective()) {
      local_alpha *= smoothstep(scale * 0.25f, scale * pow3f(0.25f), uniform_buf.pixel_fac);
    }
  }

  line.P *= scale;

  /* Clipping; restrict lines to a reasonable range for precision. */
  if (!flag_test(grid_flag, PLANE_IMAGE)) {
    float clip = drw_view_is_perspective() ?
                     grid_buf.distance :
                     (8.0f / max(drw_view().winmat[0][0], drw_view().winmat[1][1]));
    if (all(greaterThan(abs(line.P), float2(clip)))) {
      discard_line(); /* Both x, y lie outside the clip distance. */
    }
    else {
      line.P = clamp(line.P, -clip, clip);
    }
  }

  /* Add scaled camera offset, rounded to the nearest level-dependent line position. */
  line.P += round(grid_offs / scale) * scale;

  /* Clipping; restrict the grid in the UV/Image editor to the specified tile sizes */
  if (flag_test(grid_flag, PLANE_IMAGE)) {
    line.P = clamp(line.P, float2(-1.0f), 2.0f * grid_buf.size.xy - 1.0f);
  }

  /* Clipping; if there exists an integer, s.t. with the scaling of the level *above* we can draw
   * the current line, we can discard the current line on *any* sublevel as the superlevel
   * is guaranteed to draw over it. */
  if (!flag_test(grid_flag, PLANE_IMAGE) && flag_test(grid_flag, SHOW_GRID)) {
    if (line.level < OVERLAY_GRID_STEPS_DRAW - 1 && level < OVERLAY_GRID_STEPS_LEN - 1) {
      float nscale = grid_buf.level_scales[min(level + 1, OVERLAY_GRID_STEPS_LEN - 1)][0];
      float offset = round(select(grid_offs.y, grid_offs.x, line.dir) / nscale) * nscale;
      float P_diff = offset + (select(line.P.y, line.P.x, line.dir) - offset) / nscale;
      if (abs(fract(P_diff)) < 1e-5) {
        discard_line();
      }
    }
  }

  /* Output the world-space position on the correct plane. */
  local_pos = float3(0.0f);
  if (flag_test(grid_flag, SHOW_GRID)) {
    if (flag_test(grid_flag, PLANE_XY)) {
      local_pos.xy = line.P;
    }
    else if (flag_test(grid_flag, PLANE_XZ)) {
      local_pos.xz = line.P;
    }
    else if (flag_test(grid_flag, PLANE_YZ)) {
      local_pos.yz = line.P;
    }
    else { /* PLANE_IMAGE */
      local_pos.xy = line.P * 0.5f + 0.5f;
    }
  }
  else if (flag_test(grid_flag, SHOW_AXES)) {
    /* Test X/Y/Z axis flags per line */
    const uint[3] flags = {AXIS_X, AXIS_Y, AXIS_Z};
    if (!flag_test(grid_flag, flags[line.dir])) {
      discard_line();
    }
    local_pos[line.dir] = line.P.x;
  }

  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(local_pos, 1.0f));

  /* Progressively bias Z based on grid level and incline offset to address Z-fighting. */
  gl_Position.z += 4.8e-7f * float(OVERLAY_GRID_STEPS_DRAW - 1 - line.level);
  if (flag_test(grid_flag, PLANE_XY)) {
    gl_Position.z += mix(0.0f, 1.5e-4f, 1.0f - abs(drw_view_forward().z));
  }

  /* Stage output for viewport antialiasing. */
  edge_start = edge_pos = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) *
                          uniform_buf.size_viewport;
}
