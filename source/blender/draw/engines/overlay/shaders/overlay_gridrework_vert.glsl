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
/** The grid renders N sublevels, given visibility of the current level. */
#define GRID_LEVEL_OFFSET -1

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

  /* Find a line's actual level and index using a cumulative sum. */
  line.idx = vertex_id;
  for (line.level = 0; line.level < GRID_LEVELS_DRAW; line.level++) {
    if (line.idx < grid_buf.num_lines_per_level) {
      break;
    }
    line.idx -= int(grid_buf.num_lines_per_level);
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

  /* First, get the camera distance/zoom and determine `t_pos`; the point on the floor plane
   * visible to the camera center. */
  float t = get_camera_distance();
  float2 t_pos = drw_view_position().xy - t * drw_view_forward().xy;

  /* Next, set several fragment shader outputs used for smoothly fading visible grid levels:
   * - The vertex position as [-1, 1], so we can fade level boundaries.
   * - The offset for the **lowest** level, which we fade in/out.*/
  frag_xy = line_vert / float(grid_buf.num_lines_per_level >> 1);
  frag_level = line.level == 0 ? 1.0f - square(fract(base_level)) : 1.0f;

  /* Compute actual level of drawn grid data, offset negatively to focus on sub-levels. */
  line.level = int(base_level) + line.level + GRID_LEVEL_OFFSET;
  line.level = clamp(line.level, 0, GRID_LEVELS_TOTAL - 1);

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

  /* Make each level of the grid a scale larger than the previous level.
   * Additionally, move the grid with the camera in increments depending on the level. */
  //  TODO; this is a source of float imprecision
  float scale = grid_buf.level_scales[clamp(line.level, 0, GRID_LEVELS_TOTAL - 1)].x;
  float3 vertex = float3((line_vert + round(t_pos / scale)) * scale, 0.0f);

  /* Output the world-space position to the fragment stage, and cull minute scales; they are
   * hard to draw without a much larger amount of geometry. This mirrors old grid behavior. */
  local_pos = vertex;
  if (scale < 1e-3f) {
    gl_Position = float4(NAN_FLT);
  }
  else {
    gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vertex, 1.0f));
  }
}
