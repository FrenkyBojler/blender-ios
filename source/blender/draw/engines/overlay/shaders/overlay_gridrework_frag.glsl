/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

// TODO merge with vert definition
#define NUM_GRID_LEVELS     3
#define NUM_GRID_DIRECTIONS 2
#define NUM_GRID_SUBDIVS    10

void main()
{
  const float3[7] debug_colors = {
    float3(1, 0, 0),
    float3(0, 1, 0),
    float3(0, 0, 1),
    float3(1, 1, 0),
    float3(0, 1, 1),
    float3(1, 0, 1),
    float3(1, 1, 1)
  };

  float3 P = local_pos;
  float3 V = drw_view_position() - P;

  float dist = length(V);
  V /= dist;

  float angle = V.z;
  angle = 1.0f - abs(angle);
  angle *= angle;

  float fade = 1.f;

  // Fade angles
  fade *= (1.f - angle * angle);

  // Fade distances
  fade *= 1.0f - smoothstep(0.0f, 1000.f, dist - 1000.f);
  
  // TODO; fade edges
  fade *= (1.f - length(local_coord));

  // TODO; fade levels
  fade *= local_level;

  // out_color.rgb = float3(1); // mix(vec3(1, 0, 0), vec3(0, 1, 0), a);
  // out_color.rgb = debug_colors[int(local_level) % 7];
  out_color.rgb =/*  debug_colors[debug_line_lvl]; */  float3(1, 0, 1); // mix(float3(1, 0, 1), float3(1, 1, 0), float3(local_level));
  out_color.a = fade;
}