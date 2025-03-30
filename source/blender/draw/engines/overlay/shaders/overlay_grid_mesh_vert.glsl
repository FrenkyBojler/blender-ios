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
  vec3 ls_P = vec3(x, y, 0);

  gl_Position = drw_point_world_to_homogenous(ls_P);

  /* Convert to screen position [0..sizeVp]. */
  edgePos = edgeStart = ((gl_Position.xy / gl_Position.w) * 0.5 + 0.5) * sizeViewport;
}
