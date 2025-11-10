/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

VERTEX_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "gpu_shader_math_safe_lib.glsl"

/* Subdivision is a factor of 10 between levels. */
#define GRID_SUBDIVS 10 

/* The grid supports 3 hardcoded levels of hierarchy. */
#define GRID_LEVELS 3

/* Specify orders of magnitude of grid-sublevels visible at normal scale. */
#define GRID_LEVEL_OFFSET -2

/* Disable support for infinite zoom-in. */
// #define GRID_FINITE_LEVELS

/** Keep in sync with `SI_GRID_STEPS_LEN` in `DNA_space_types.h`. */
#define STEPS_LEN 8

/* Helper struct; encodes specific information about an implicitly defined vertex.
 * See `init_grid_line_data()` below for construction. */
struct GridLineData {
  uint idx;
  uint side;
  uint direction;
  int level;
};

GridLineData init_grid_line_data(in uint vertex_id)
{
  GridLineData data;

  /* Every pair of consecutive vertices forms a line, indicated by bit 0. */
  data.side = vertex_id & 0x1;
  vertex_id = vertex_id >> 1;

  /* Every pair of consecutive lines alternates x/y direction, indicated by bit 1. */
  data.direction = vertex_id & 0x1;
  vertex_id = vertex_id >> 1;

  /* Line counts per level are packed, so we find a line's actual index/level by
   * doing a cumulative sum. */
  /* TODO: Clean this up, can be simpler. */
  data.idx = vertex_id;
  for (int i = 0; i < GRID_LEVELS; ++i) {
    if (data.idx < grid_buf.num_lines_per_level) {
      data.level = i;
      break;
    } else {
      data.idx -= int(grid_buf.num_lines_per_level);
    }
  }

  return data;
}

/* Determine the grid level offset based on camera distance or scale `t`. */
float grid_distance_level_offset(in float t) {
  /* Logarithmic scale: `log10(t) = log2(t) / log2(10)`. */
  float level_offset = log2(t) / log2(float(GRID_SUBDIVS)); 

  /* GRID_LEVEL_OFFSET` offsets the lowest level by this value, mirroring grid behavior pre 5.1, 
   * where 1 order of magnitude lower is always visible. */
  level_offset += GRID_LEVEL_OFFSET;

  /* `GRID_FINITE_LEVELS` enforces a minimum sublevel; infinite zoom may be confusing during 
   * scene navigation and does not mirror grid behavior pre 5.1.  */
#ifdef GRID_FINITE_LEVELS
  if (drw_view_is_perspective()) {
    level_offset = max(-0.99f, level_offset);
  }
#endif

  return level_offset;
}

void main()
{
  /* TODO; identify if we are drawing an infinite grid, or a specific set of lines */

  /* This vertex is part of the infinite grid. We extract data from gl_VertexID. */
  GridLineData grid_data = init_grid_line_data(gl_VertexID);

  /* We next calculate the vertex position at the start/end of a line as a value in [-1, 1]. */
  float vert_y = float(grid_buf.num_lines_per_level >> 1);
  float vert_x = float(grid_data.idx) - vert_y;
  float3 vert_pos = float3(vert_x, vert_y, 0.0f);
  /* If this isn't the start vertex, flip the y-coord to define the end vertex. */
  vert_pos.y = select(vert_pos.y, -vert_pos.y, grid_data.side);
  /* If this isn't the x-direction, flip coordinates to define the y-direction. */
  vert_pos.xy = select(vert_pos.xy, vert_pos.yx, grid_data.direction);

  /* We now adjust grid levels dependent on a scale value `t`. */
  float t;
  if (drw_view_is_perspective()) {
    /* Scale depends on distance to a point on the floor plane; we interpolate between the point
     * viewed in the camera center and the point directly below the camera, dependent on azimuth. */
    t = mix(abs(drw_view_position().z / drw_view_forward().z), 
      abs(drw_view_position().z), 1.0f - abs(drw_view_forward().z));
  } else {
    /* Scale simply depends on x/y-scaling of the orthographic camera */
    t = 0.5f / min(drw_view().winmat[0][0], drw_view().winmat[1][1]);
  } 
  float level_offset = grid_distance_level_offset(t);

  /* Fragment shader outputs for the alpha component:
   * - The vertex position as [-1, 1], so we can fade level boundaries.
   * - The level offset for the lowest level so we can fade it in/out. */
  frag_xy = vert_pos.xy / vert_y;
  frag_level = select(1.0f, float(grid_data.level + 1) - fract(level_offset), grid_data.level == 0);

  /* Dependent on the grid flag, we place the grid on the correct plane. */
  // if (flag_test(grid_flag, PLANE_XY)) {
  //   vert_pos = float3(vert_pos.x, vert_pos.y, 0.0f);
  // }
  // else if (flag_test(grid_flag, PLANE_XZ)) {
  //   vert_pos = float3(vert_pos.x, 0.0f, vert_pos.y);
  // }
  // else if (flag_test(grid_flag, PLANE_YZ)) {
  //   vert_pos = float3(0.0f, vert_pos.x, vert_pos.y);
  // }
  // else { /* PLANE_IMAGE */ {
  //   vert_pos = float3(vert_pos.xy * 0.5f + 0.5f, 0.0f);
  // }

  /* Grid levels can have different scales dependent on system units; we render these scales 
   * dependent on which levels are active. We loop through the scales to find a relevant offset. */
  int base_grid_level = 0;
  for (; base_grid_level < STEPS_LEN; base_grid_level++) {
    if (grid_buf.level_scales[base_grid_level].x > 1.0f) {
      base_grid_level -= 1;
      break;
    }
  }
  /* Round to the nearest upper level. */
  grid_data.level += base_grid_level + int(ceil(level_offset));
  
  /* Make each level of the grid an order of magnitude larger than the previous level.
   * Additionally, move the grid with the camera in increments depending on the level. */
  float vert_scale = grid_buf.level_scales[grid_data.level].x;
  float3 vert_offset = float3(drw_view_position().xy + t * -drw_view_forward().xy, 0);
  vert_pos = (vert_pos + round(vert_offset / vert_scale)) * vert_scale;

  /* Fragment output */
  local_pos = vert_pos;

  /* We cull vertex scale below absurdist values; they are hard to draw with geometry for now. */
  if (vert_scale < 1e-3f) {
    gl_Position = float4(NAN_FLT);
  } else {
    gl_Position = drw_view().winmat * (drw_view().viewmat * float4(vert_pos, 1.0f));
  }
}