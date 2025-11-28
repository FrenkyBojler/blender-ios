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

  /* The index/level of a line are encoded by the remaining bits. We order
   * levels from "large" to "small", drawing the larger levels first. */
  line.level = OVERLAY_GRID_STEPS_DRAW - 1 - int(vertex_id / grid_buf.num_lines);
  vertex_id = vertex_id % grid_buf.num_lines;

  /* From the index, generate N+1 points equidistantly spaced on [-N/2, N/2]. */
  line.P.x = max(float(grid_buf.num_lines >> 1u), 1.0f);
  line.P.y = (float(vertex_id) - float(grid_buf.num_lines >> 1u));

  /* If this isn't the line start, flip the x-component to the end. Likewise,
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
   * They then alternate x/y/z, indicated by the remaining bits. */
  uint side = vertex_id & 0x1u;
  line.axis = vertex_id >> 1u;
  /* For an axis line, the level is fixed, and the direction is simply the vertex index. */
  line.level = OVERLAY_GRID_STEPS_DRAW - 1;
  /* Output a vertex as [-N/2, N/2], [0, 0]. */
  line.P.x = max(float(grid_buf.num_lines >> 1u), 1.0f) * select(1.0f, -1.0f, side);
  line.P.y = 0.0f;

  return line;
}

/* Returns true if components of `v` fall within `epsilon` of 0. */
bool2 is_zero(in float2 v, in float epsilon) {
  return lessThanEqual(abs(v), float2(epsilon));
}

/* Test if the current line falls under an active axis line which occludes it. */
bool test_axis_occlude(in float3 vertex_pos_global) {
  if (flag_test(grid_flag, SHOW_GRID)) {
    return (flag_test(grid_flag, AXIS_X) && all(is_zero(vertex_pos_global.yz, 1e-4f))) ||
           (flag_test(grid_flag, AXIS_Y) && all(is_zero(vertex_pos_global.xz, 1e-4f))) ||
           (flag_test(grid_flag, AXIS_Z) && all(is_zero(vertex_pos_global.xy, 1e-4f)));
  }
  return false;
}

/* Test if the current line falls under another line on a higher level, which occludes it. */
bool test_level_occlude(in LineData line, in uint level) {
  if (flag_test(grid_flag, SHOW_GRID)) {
    if (line.level < OVERLAY_GRID_STEPS_DRAW - 1 && level < OVERLAY_GRID_STEPS_LEN - 1) {
      float step_size_curr = grid_buf.steps[level][line.axis];
      float step_size_next = grid_buf.steps[level + 1][line.axis];

      float2 step_offs_curr = round(grid_buf.offset / step_size_curr) * step_size_curr;
      float2 step_offs_next = round(step_offs_curr / step_size_next) * step_size_next;
      float2 diff = step_offs_next + (line.P - step_offs_next) / step_size_next;

      if (is_equal(fract(diff[1 - line.axis]), 0.0f, 1e-4)) {
        return true;
      }
    }
  }
  return false;
}

void main()
{
  gl_Position = float4(NAN_FLT); /* Discard by default. */
  LineData line = flag_test(grid_flag, SHOW_GRID) ? decode_grid_data(gl_VertexID) :
                                                    decode_axis_data(gl_VertexID);

  /* Compute the actual level of a line, offset by -1 to force a sublevel in the 3D viewport. */
  int level = int(grid_buf.level) + int(line.level) - (flag_test(grid_flag, GRID_SIMA) ? 0 : 1);
  if (level < 0 || level >= OVERLAY_GRID_STEPS_LEN) {
    return; /* Discard line. */
  }

  /* Compute per-level size, camera offset for lines. Offset is rounded to the nearest
   * level-dependent line position for  grid, while axes simply move with the camera. */
  float step_size = grid_buf.steps[level][line.axis];
  float2 step_offs = flag_test(grid_flag, SHOW_GRID) /* !SHOW_AXES */
    ? round(grid_buf.offset / step_size) * step_size
    : float2(drw_view_position()[line.axis], 0.0f);

  /* Output vertex position in [-1,1], which we use to fade level boundaries. */
  vertex_out.coord = line.P / max(float(grid_buf.num_lines >> 1), 1.0f);
  /* Output level fade in [0, 1], which we use to smoothly transition grid levels. */
  vertex_out_flat.alpha = (line.level + 1.0f - fract(grid_buf.level)) /
                     float(OVERLAY_GRID_STEPS_DRAW - 1);
  vertex_out_flat.alpha = saturate(vertex_out_flat.alpha);
  if (!drw_view_is_perspective()) {
    /* Fade by pixel size for orthographic, as we lack proper line dfdx/dfdy. */
    vertex_out_flat.alpha *= smoothstep(
        step_size * 0.25f, step_size * pow3f(0.25f), uniform_buf.pixel_fac);
  }

  /* Apply per-level size, camera offset. */
  line.P = step_offs + step_size * line.P;

  /* Lines are clamped to a clipping rectangle to avoid precision issues further on. */
  if (flag_test(grid_flag, GRID_SIMA)) {
    /* Restrict the grid in the UV/Image editor to the specified tile size. */
    line.P = clamp(line.P, float2(-1.0f), grid_buf.clip_rect * 2.0f - 1.0f);
  }
  else {
    bool line_outside_rect = all(greaterThan(abs(line.P - step_offs), grid_buf.clip_rect)); 
    if (line_outside_rect) {
      return; /* Discard line. */
    }
    line.P = clamp(line.P, grid_buf.offset - grid_buf.clip_rect, grid_buf.offset + grid_buf.clip_rect);
  }

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
    else { /* GRID_SIMA */
      vertex_out.pos.xy = line.P * 0.5f + 0.5f;
      /* Set z to place the grid over/under image, and always under the UV mesh. 
       * See `overlay_edit_uv_edges_vert.glsl` for the full z-sorder. */
      vertex_out.pos.z = flag_test(grid_flag, GRID_OVER) ? 0.74f : 0.76f;
    }
  }
  else /* if (flag_test(grid_flag, SHOW_AXES)) */ { /* SHOW_AXES */
    /* Test X/Y/Z axis flags per line */
    const uint axis_flags[3] = {AXIS_X, AXIS_Y, AXIS_Z};
    if (!flag_test(grid_flag, axis_flags[line.axis])) {
      return; /* Discard line. */
    }
    vertex_out.pos[line.axis] = line.P.x;
  }
  
  /* Cull occluded lines. */
  if (test_axis_occlude(vertex_out.pos) || test_level_occlude(line, level)) {
    return;
  }

  gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vertex_out.pos, 1.0f));

  /* Progressively bias Z based on grid level and incline offset to address Z-fighting. */
  if (flag_test(grid_flag, SHOW_GRID)) {
    gl_Position.z += 4.8e-7f * float(OVERLAY_GRID_STEPS_DRAW - line.level);
  }
  /* if (flag_test(grid_flag, PLANE_XY)) {
    gl_Position.z += mix(0.0f, 1.5e-4f, 1.0f - abs(drw_view_forward().z));
  } */

  /* Stage output for viewport antialiasing. */
  edge_start = edge_pos = ((gl_Position.xy / gl_Position.w) * 0.5f + 0.5f) * uniform_buf.size_viewport;
}
