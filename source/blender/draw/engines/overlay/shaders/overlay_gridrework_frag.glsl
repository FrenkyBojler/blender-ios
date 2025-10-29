/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  const float3[4] debug_colors = {
    float3(1, 0, 0),
    float3(0, 1, 0),
    float3(0, 0, 1),
    float3(1, 0, 1)
  };

  float3 P = local_pos;
  float3 V = drw_view_position() - P;

  float dist = length(V);
  V /= dist;

  float angle = V.z;
  angle = 1.0f - abs(angle);
  angle *= angle;

  float fade = 1.0f - angle * angle;
  fade *= 1.0f - smoothstep(0.0f, 512.f, dist - 512.f);
  
  // out_color.rgb = debug_colors[local_lvl % 4u];
  out_color.rgb = float3(0); // local_pos;
  out_color.a   = 1.f; /* fade */;
}