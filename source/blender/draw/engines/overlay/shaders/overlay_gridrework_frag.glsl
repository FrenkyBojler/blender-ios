/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  /* Compute normalized view vector. */
  float3 P = local_pos;
  float3 V = drw_view_position() - P;
  float dist = length(V);
  V /= dist;

  /* Output alpha value. Modified below */
  float fade = 1.f;

  /* Add fade at steep angles. */
  float angle = V.z;
  angle = 1.0f - abs(angle);
  angle *= angle;
  fade *= (1.f - angle * angle);

  /* Add stepped fade towards clip distance. */
  fade *= 1.0f - smoothstep(0.0f, grid_buf.distance, dist - grid_buf.distance);

  /* Add fade towards edge of current level's edge. */
  float length_fade = 1.f - min(1.f, dot(frag_xy, frag_xy));
  fade *= length_fade * length_fade;

  /* Add fade at level switch. This is computed in the vertex stage. */
  fade *= frag_level;

  out_color.rgb = float3(1, 0, 1); 
  out_color.a = fade;
}