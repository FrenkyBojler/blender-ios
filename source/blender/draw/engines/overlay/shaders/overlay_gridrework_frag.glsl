/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  float3 P = local_pos;
  float3 V = drw_view_position() - P;
  
  float fade = 1.f;

  // Fade by distance
  float dist = length(V);
  V /= dist;
  fade *= 1.0f - smoothstep(0.0f, 500.f, dist - 500.f); // fade at draw distance

  // Fade at angle
  float angle = V.z;
  angle = 1.0f - abs(angle);
  angle *= angle;
  fade *= (1.f - angle * angle);                        // fade at steep angles

  // Fade at edge of level draw
  fade *= (1.f - length(frag_xy));                      // fade at draw edge

  // Fade at level switch
  fade *= frag_level;                                   // fade at level switch

  out_color.rgb = float3(1, 0, 1); 
  out_color.a = fade;
}