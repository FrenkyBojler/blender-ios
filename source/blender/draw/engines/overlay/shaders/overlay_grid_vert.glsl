/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_grid_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

struct LineData {
  float2 P;
  uint axis;  /* [0, 1, 2] */
  uint level; /* [0, ..., OVERLAY_GRID_STEPS_DRAW - 1] */
};

/* Helper; gl_VertexID implicitly encodes a grid line. */
LineData decode_grid_data(in uint vertex_id)
{
  LineData line;

  /* Every pair of consecutive verts forms a line, indicated by bit 0.
   * Every pair of consecutive lines flips x/y, indicated by bit 1. */
  uint side = vertex_id & 0x1u;
  vertex_id = vertex_id >> 1u;
  line.axis = vertex_id & 0x1u;
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
  line.P.x = side != 0 ? line.P.x : -line.P.x;
  line.P.xy = line.axis != 0 ? line.P.yx : line.P.xy;

  return line;
}

/* Helper; gl_VertexID implicitly encodes one of three axis lines. */
LineData decode_axis_data(in uint vertex_id)
{
  LineData line;

  /* Every pair of consecutive verts forms a line, indicated by bit 0.
   * They then alternate x/y/z, indicated by the other 31 bits. */
  uint side = vertex_id & 0x1u;
  line.axis = vertex_id >> 1u;
  /* For an axis line, the level is fixed, and the direction is simply the vertex index. */
  line.level = OVERLAY_GRID_STEPS_DRAW - 1;
  /* Output a vertex as [-N/2, N/2], [0, 0]. */
  line.P.x = max(float(grid_buf.num_lines >> 1u), 1.0f) * select(1.0f, -1.0f, side);
  line.P.y = 0.0f;

  return line;
}

void main()
{
  /* Discard by default. */
  gl_Position = float4(NAN_FLT);

  LineData line = flag_test(grid_flag, SHOW_GRID) ? decode_grid_data(gl_VertexID) :
                                                    decode_axis_data(gl_VertexID);

  /* Compute the actual level of a line, offset by -1 to force a sublevel in the 3D viewport. */
  int level = int(grid_buf.level) + int(line.level) - (flag_test(grid_flag, PLANE_IMAGE) ? 0 : 1);
  if (level < 0 || level >= OVERLAY_GRID_STEPS_LEN) {
    return; /* Discard line. */
  }

  /* Compute per-level size, camera offset for lines. Note that offset is rounded to the nearest
   * level-dependent line position. */
  float step_size = grid_buf.steps[level][line.axis];
  float2 step_offs = round(grid_offs / step_size) * step_size;

  /* Output vertex position in [-1,1], which we use to fade level boundaries. */
  vertex_out.coord = line.P / max(float(grid_buf.num_lines >> 1), 1.0f);
  /* Output level fade in [0, 1], which we use to smoothly transition grid levels. */
  vertex_out.alpha = (line.level + 1.0f - fract(grid_buf.level)) /
                     float(OVERLAY_GRID_STEPS_DRAW - 1);
  vertex_out.alpha = saturate(vertex_out.alpha);
  /* Fade by pixel size for orthographic, as we lack proper line dfdx/dfdy. */
  if (!drw_view_is_perspective()) {
    vertex_out.alpha *= smoothstep(
        step_size * 0.25f, step_size * pow3f(0.25f), uniform_buf.pixel_fac);
  }

  /* Apply per-level size, camera offset. */
  line.P = step_offs + step_size * line.P;

  if (flag_test(grid_flag, PLANE_IMAGE)) {
    /* Clipping; restrict the grid in the UV/Image editor to the specified tile size. */
    line.P = clamp(line.P, float2(-1.0f), grid_buf.clip_rect * 2.0f - 1.0f);
  }
  else {
    /* Clipping; restrict lines to a reasonable range for precision. */
    if (all(greaterThan(abs(line.P - step_offs), grid_buf.clip_rect))) {
      return; /* Discard line. */
    }
    line.P = clamp(line.P, step_offs - grid_buf.clip_rect, step_offs + grid_buf.clip_rect);
  }

  /* Clipping; if there exists an integer, s.t. with the scaling of the level above we can draw
   * the current line, we can discard the current line on any sublevel. */
  /* TODO (not_mark): re-enable when I can work out problems with this */
  // if (!flag_test(grid_flag, PLANE_IMAGE) && flag_test(grid_flag, SHOW_GRID)) {
  //   if (line.level < OVERLAY_GRID_STEPS_DRAW - 1 && level < OVERLAY_GRID_STEPS_LEN - 1) {
  //     float nscale = grid_buf.steps[min(level + 1, OVERLAY_GRID_STEPS_LEN - 1)][0];
  //     float offset = round(select(grid_offs.y, grid_offs.x, line.axis) / nscale) * nscale;
  //     float P_diff = offset + (select(line.P.y, line.P.x, line.axis) - offset) / nscale;
  //     if (abs(fract(P_diff)) < 1e-5) {
  //       return;  /* Discard line. */
  //     }
  //   }
  // }

  /* Output world-space position on the correct plane/axis. */
  vertex_out.pos = float3(0.0f);
  if (flag_test(grid_flag, SHOW_GRID)) {
    if (flag_test(grid_flag, PLANE_XY)) {
      vertex_out.pos.xy = line.P;
    }
    else if (flag_test(grid_flag, PLANE_XZ)) {
      vertex_out.pos.xz = line.P;
    }
    else if (flag_test(grid_flag, PLANE_YZ)) {
      vertex_out.pos.yz = line.P;
    }
    else { /* PLANE_IMAGE */
      vertex_out.pos.xy = line.P * 0.5f + 0.5f;
    }
  }
  else if (flag_test(grid_flag, SHOW_AXES)) {
    /* Test X/Y/Z axis flags per line */
    const uint flags[3] = {AXIS_X, AXIS_Y, AXIS_Z};
    if (!flag_test(grid_flag, flags[line.axis])) {
      return; /* Discard line. */
    }
    vertex_out.pos[line.axis] = line.P.x;
  }

  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vertex_out.pos, 1.0f));

  /* Progressively bias Z based on grid level and incline offset to address Z-fighting. */
  gl_Position.z += 4.8e-7f * float(OVERLAY_GRID_STEPS_DRAW - 1 - line.level);
  if (flag_test(grid_flag, PLANE_XY)) {
    gl_Position.z += mix(0.0f, 1.5e-4f, 1.0f - abs(drw_view_forward().z));
  }

  /* Stage output for viewport antialiasing. */
  vertex_out.edge_start = vertex_out.edge_pos = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) *
                                                uniform_buf.size_viewport;
}
