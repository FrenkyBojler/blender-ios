/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

/** The grid uses N hardcoded levels of hierarchy. */
#define GRID_LEVELS_DRAW 4
/** Keep in sync with `SI_GRID_STEPS_LEN` in `DNA_space_types.h`. */
#define GRID_LEVELS_TOTAL 8

/* Helper struct; encodes specific information about an implicitly defined vertex.
 * See `get_line_data()` and `get_line_vertex()` below. */
struct LineData {
  uint idx;
  uint side;
  uint direction;
  int level;
};

LineData get_line_data(in uint vertex_id)
{
  LineData line;

  /* Every pair of consecutive vertices forms a line, indicated by bit 0. */
  line.side = vertex_id & 0x1;
  vertex_id = vertex_id >> 1;

  /* Every pair of consecutive lines alternates x/y direction, indicated by bit 1. */
  line.direction = vertex_id & 0x1;
  vertex_id = vertex_id >> 1;

  /* Find a line's actual index/level by doing a cumulative sum. */
  line.idx = vertex_id;  // TODO: Clean this up, can be simpler.
  for (int i = 0; i < GRID_LEVELS_DRAW; ++i) {
    if (line.idx < grid_buf.num_lines_per_level) {
      line.level = i;
      break;
    }
    else {
      line.idx -= int(grid_buf.num_lines_per_level);
    }
  }

  return line;
}

float2 get_line_vertex(in LineData line)
{
  float2 vertex;

  /* Generate a set of N+1 points equidistantly spaced between [-N/2, N/2]. */
  vertex.y = float(grid_buf.num_lines_per_level >> 1);
  vertex.x = float(line.idx) - vertex.y;

  /* If this isn't the start vertex, flip the y-coord to define the end vertex. */
  vertex.y = select(vertex.y, -vertex.y, line.side);

  /* If this isn't the x-direction, flip x/y-coords to define the y-direction. */
  vertex.xy = select(vertex.xy, vertex.yx, line.direction);

  return vertex;
}

float get_camera_distance()
{
  float t;
  if (drw_view_is_perspective()) {
    /* Scale depends on distance to a point on the floor plane; we interpolate between the point
     * viewed by the camera and the point directly below it, dependent on azimuth. */
    t = mix(abs(drw_view_position().z / drw_view_forward().z),
            abs(drw_view_position().z),
            1.0f - abs(drw_view_forward().z));
  }
  else {
    /* Scale simply depends on x/y-scaling of the orthographic camera */
    t = 1.0f / min(drw_view().winmat[0][0], drw_view().winmat[1][1]);
  }
  return t;
}

void main()
{
  /* We extract a vertex - part of an infinite grid - from gl_VertexID. */
  // TODO; identify if we are drawing an infinite grid, or a bounded set of lines
  LineData line = get_line_data(gl_VertexID);
  float2 line_vert = get_line_vertex(line);

  /* We now adjust grid levels dependent on camera zoom, using a base 10 scale. */
  /* Next, get the camera distance/zoom. */
  float t = get_camera_distance();
  /* We determine `t_level`; the level adjustment dependent on camera zoom. */
  // float t_scale = grid_scale >= 1.0f ? 1.0f / grid_scale : grid_scale;
  float t_level = log2(t) / log2(10); /* Camera zoom is base-10 logarithmic. */

  /* We also determine `t_pos`; the point on the floor plane visible to the camera center. */
  float2 t_pos = drw_view_position().xy - t * drw_view_forward().xy;

  /* Next, set several fragment shader outputs used for smoothly fading visible grid levels:
   * - The vertex position as [-1, 1], so we can fade level boundaries.
   * - The offset for the **lowest** level, which we fade in/out. */
  frag_xy = line_vert / float(grid_buf.num_lines_per_level >> 1);
  frag_level = line.level > 0 ? 1.0f : 1.0f - fract(t_level);

  /* int base_offset = 0;
  for (base_offset = 0; base_offset < GRID_LEVELS_TOTAL; ++base_offset) {

  }*/

  /* Compute actual level of grid data. Offset negatively to show 2 sublevels when available. */
  line.level = line.level + base_level_offset +
               int(floor(t_level)) /* int(ceil(t_level))  */ /* - 2 */;
  line.level = clamp(line.level, 0, GRID_LEVELS_TOTAL - 1);

  /* Make each level of the grid a scale larger than the previous level.
   * Additionally, move the grid with the camera in increments depending on the level. */
  float scale = grid_buf.level_scales[line.level].x;
  float3 vertex = float3((line_vert + round(t_pos / scale)) * scale, 0.0f);
  // float3 vertex = float3(line_vert * scale, 0.0f);

  if (gl_VertexID == 0) {
    printf("t: %f, level: %d, scale: %f\n", t, line.level, scale);
  }

  /* Dependent on the grid flag, we now swap the grid on the correct plane. */
  // if (flag_test(grid_flag, PLANE_XY)) {
  //   line_vert = float3(line_vert.x, line_vert.y, 0.0f);
  // }
  // else if (flag_test(grid_flag, PLANE_XZ)) {
  //   line_vert = float3(line_vert.x, 0.0f, line_vert.y);
  // }
  // else if (flag_test(grid_flag, PLANE_YZ)) {
  //   line_vert = float3(0.0f, line_vert.x, line_vert.y);
  // }
  // else { /* PLANE_IMAGE */ {
  //   line_vert = float3(line_vert.xy * 0.5f + 0.5f, 0.0f);
  // }

  /* Fragment output */
  local_pos = vertex;

  /* We cull vertex scales below 1e-3 values; they are hard to draw without a large amount of
   * geometry. */
  if (scale < 1e-3f) {
    gl_Position = float4(NAN_FLT);
  }
  else {
    gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vertex, 1.0f));
  }
}
