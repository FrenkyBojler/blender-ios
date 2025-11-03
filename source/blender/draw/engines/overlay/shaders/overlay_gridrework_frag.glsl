/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  // Compute normalized view vector
  float3 P = local_pos;
  float3 V = drw_view_position() - P;
  float dist = length(V);
  V /= dist;

  // Output alpha
  float fade = 1.f;

  // Fade for fragment size
  // float3 dFdxPos = gpu_dfdx(P);
  // float3 dFdyPos = gpu_dfdy(P);
  // // float3 fwidthPos = abs(dFdxPos) + abs(dFdyPos);
  // float fsizePos = length(dFdxPos + dFdyPos); // 1.f - length(fwidthPos.xy); // dot(fwidthPos.xy, fwidthPos.xy);
  // fsizePos *= fsizePos;
  // size *= size;
  // fade *= size;

  // Fade at angle
  float angle = V.z;
  angle = 1.0f - abs(angle);
  // angle *= angle;
  fade *= (1.f - angle * angle);

  // Fade towards draw distance
  fade *= 1.0f - smoothstep(0.0f, grid_buf.distance, dist - grid_buf.distance);

  // Fade towards edge of current level's lines
  float length_fade = 1.f - length(frag_xy);
  length_fade *= length_fade;
  // length_fade *= length_fade;
  fade *= (1.f - length(frag_xy));

  // Fade at level switch
  if (debug_grid_lvl == 2) {
    float edge_fade = length(frag_xy);
    edge_fade *= edge_fade;
    fade *= mix(frag_level, 1.0f, 1.0f - edge_fade);
  } else {
    fade *= frag_level;
  }

  float3 debug_rgb[4] = {
    float3(1, 0, 0),
    float3(0, 1, 0),
    float3(0, 0, 1),
    float3(1, 1, 1)
  };

  // out_color.rgb = float3(length_fade, 0, 0); // mix(float3(1, 0, 0), float3(0, 1, 0), size);
  // out_color.rgb = debug_rgb[debug_grid_lvl % 3];
  out_color.rgb = float3(1, 0, 1); 
  out_color.a = fade;
}