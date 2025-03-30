/* SPDX-FileCopyrightText: 2017-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_info.hh"

VERTEX_SHADER_CREATE_INFO(overlay_grid_mesh)

/**
 * Procedural mesh grid
 */

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

void main()
{
  int x = int(uint(gl_VertexID) >> 16u) - 0x7FFF;
  int y = int(uint(gl_VertexID) & (~0x0u >> 16u)) - 0x7FFF;
  vec3 ls_P = vec3(x, y, 0.0) * unit_scale;

  float snap_to = next_divider * unit_scale;
  /* Round to grid increment. */
  vec3 camera_P = drw_view_position();
  ls_P.xy -= fract(camera_P.xy / snap_to) * snap_to;
  ls_P.z -= camera_P.z;

  /* Do not use matrix translation as it degrades precision. */
  vec3 vs_P = drw_normal_world_to_view(ls_P);

  /* TODO(fclem): Coloring for axes. */
  /* TODO(fclem): Fade depending on fwidth. */
  finalColor = vec4(vec3(unit_scale / 64.0), 1.0);

  gl_Position = drw_point_view_to_homogenous(vs_P);

  /* Convert to screen position [0..sizeVp]. */
  edgePos = edgeStart = ((gl_Position.xy / gl_Position.w) * 0.5 + 0.5) * sizeViewport;
}
