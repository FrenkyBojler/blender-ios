/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

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

LineData decode_axis_data(in uint vertex_id) {
  LineData line;
  uint side = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;
  line.dir = 0;
  line.level = OVERLAY_GRID_STEPS_LEN - 1;
  line.P.x = max(float(num_lines >> 1u), 1.0f) * select(1.0f, -1.0f, side);
  line.P.y = 0.0f;
  return line;
}

/* Helper; gl_VertexID implicitly encodes a grid line; this is used for drawing the entire
 * grid plus two axes on the same plane plane. */
LineData decode_line_data(in uint vertex_id)
{
  LineData line;

  /* Every pair of consecutive verts forms a line, indicated by bit 0.
   * Every pair of consecutive lines flips x/y, indicated by bit 1. */
  uint side = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;
  line.dir = vertex_id & 0x1u; /* Stored for later use. */
  vertex_id = vertex_id >> 1u;

  /* The index/level of a line are encoded by the 30 remaining bits. Note that we order
   * levels from "large" to "small", prioritizing output of the larger levels. */
  line.level = (OVERLAY_GRID_STEPS_DRAW - 1 - int(vertex_id / num_lines));
  vertex_id = vertex_id % num_lines;

  /* From the index, generate N+1 points equidistantly spaced on [-N/2, N/2]. */
  line.P.x = max(float(num_lines >> 1u), 1.0f);
  line.P.y = (float(vertex_id) - float(num_lines >> 1u));

  /* If this isn't the start of the line, flip the x-component to the end. Likewise,
   * if this isn't the x-direction, flip components to define the y-direction. */
  line.P.x = select(line.P.x, -line.P.x, side);
  line.P.xy = select(line.P.xy, line.P.yx, line.dir);
  
  return line;
}

void main()
{
  /* If the Z-axis is to be drawn, we offset the vertex id by 2. */
  const uint vertex_id = (flag_test(grid_flag, SHOW_AXIS_Z) && gl_VertexID > 1) 
    ? gl_VertexID - 2 : gl_VertexID;
  LineData line = (flag_test(grid_flag, SHOW_AXIS_Z) && gl_VertexID > 1)
    ? decode_line_data(vertex_id) : decode_axis_data(vertex_id);

  /* Compute the actual level of a line, offset by -1 to force a sublevel in the 3D viewport. */
  int level = int(grid_level) + line.level - (flag_test(grid_flag, PLANE_IMAGE) ? 0 : 1);
  if (level < 0 || level >= OVERLAY_GRID_STEPS_LEN) {
    discard_line();
  }

  /* Scale lines appropriately based on level scaling. */
  float scale = grid_buf.level_scales[level][line.dir];

  /* Stage outputs. */
  {
    /* Stage output: vertex position in [-1,1], which we use to fade level boundaries. */
    local_coord = line.P / max(float(num_lines >> 1), 1.0f);

    /* Stage output: level fade in [0, 1], which we use to smoothly transition grid levels. */
    local_alpha = saturate(line.level + 1.0f - fract(grid_level)) /
                  float(OVERLAY_GRID_STEPS_DRAW - 1);
    local_alpha = 2.0f * local_alpha - square(local_alpha); /* Upside down parabola curve. */
    
    /* Fade by pixel size for orthographic, as we lack proper line dfdx/dfdy. */
    if (!drw_view_is_perspective()) {
      local_alpha *= smoothstep(scale * 0.25f, scale * pow3f(0.25f), uniform_buf.pixel_fac);
    }
  }

  line.P *= scale;

  /* Clipping; restrict lines to a reasonable range for float precision, as large lines can lead 
   * to flickering/teleporting. */
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
  if (flag_test(grid_flag, SHOW_GRID) && vertex_id != gl_VertexID) { /* Not the Z-axis. */
    line.P += round(grid_poi / scale) * scale;
  }

  /* Clipping; restrict the grid in the UV/Image editor to the specified tile sizes */
  if (flag_test(grid_flag, PLANE_IMAGE)) {
    line.P = clamp(line.P, float2(-1.0f), 2.0f * grid_buf.size.xy - 1.0f);
  }

  /* Clipping; if there exists an integer, s.t. with the scaling of the level *above* we can draw
   * the current line, we can discard the current line on *any* sublevel as the superlevel
   * is guaranteed to draw over it. */
  if (!flag_test(grid_flag, PLANE_IMAGE)) {
    if (line.level < OVERLAY_GRID_STEPS_DRAW - 1 && level < OVERLAY_GRID_STEPS_LEN - 1) {
      float nscale = grid_buf.level_scales[min(level + 1, OVERLAY_GRID_STEPS_LEN - 1)][0];
      float offset = round(select(grid_poi.y, grid_poi.x, line.dir) / nscale) * nscale;
      float P_diff = offset + (select(line.P.y, line.P.x, line.dir) - offset) / nscale;
      if (abs(fract(P_diff)) < 1e-5) {
        discard_line();
      }
    }
  }

  /* Output the world-space position on the correct plane. */
  if (flag_test(grid_flag, SHOW_AXIS_Z) && vertex_id == gl_VertexID) { /* Z-axis */
    local_pos = float3(0.0f, 0.0f, line.P.x);
  }
  else if (flag_test(grid_flag, PLANE_XY)) {
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

  /* Progressively bias output based on grid level and incline offset to address z-fighting. */
  float z_offset = 4.8e-7f * float(OVERLAY_GRID_STEPS_DRAW - 1 - line.level);
  if (flag_test(grid_flag, PLANE_XY)) {
    z_offset += mix(0.0f, 1.5e-4f, 1.0f - abs(drw_view_forward().z));
  }
  gl_Position.z += z_offset;

  // if (gl_VertexID == vertex_id) {
  //   printf(" %d, [%f, %f, %f, %f]\n", gl_VertexID, gl_Position.x, gl_Position.y, gl_Position.z, gl_Position.w);
  // }

  /* Stage output: variables for viewport antialiasing. */
  edge_start = edge_pos = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) *
                          uniform_buf.size_viewport;
}
