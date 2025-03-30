/* SPDX-FileCopyrightText: 2017-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_info.hh"

VERTEX_SHADER_CREATE_INFO(overlay_grid_mesh)

/**
 * Procedural mesh grid
 */

#include "draw_view_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

vec4 get_homogenous_space_grid_point(int x, int y)
{
  vec3 ls_P = vec3(x, y, 0.0) * unit_scale;

  float snap_to = next_divider * unit_scale;
  /* Round to grid increment. */
  vec3 camera_P = drw_view_position();
  ls_P.xy -= fract(camera_P.xy / snap_to) * snap_to;
  ls_P.z -= camera_P.z;

  /* Do not use matrix translation as it degrades precision. */
  vec3 vs_P = drw_normal_world_to_view(ls_P);

  return drw_point_view_to_homogenous(vs_P);
}

void main()
{
  int x = int(uint(gl_VertexID) >> 16u) - 0x7FFF;
  int y = int(uint(gl_VertexID) & (~0x0u >> 16u)) - 0x7FFF;

  vec4 hs_P = get_homogenous_space_grid_point(x, y);
  /* Adjacent points used to get screen space grid density. */
  vec4 hs_P_dx = get_homogenous_space_grid_point(x + 1, y);
  vec4 hs_P_dy = get_homogenous_space_grid_point(x, y + 1);

  /* Convert to screen position [0..sizeVp]. */
  vec2 ss_P = drw_ndc_to_screen(drw_perspective_divide(hs_P)).xy * sizeViewport;
  vec2 ss_P_dx = drw_ndc_to_screen(drw_perspective_divide(hs_P_dx)).xy * sizeViewport;
  vec2 ss_P_dy = drw_ndc_to_screen(drw_perspective_divide(hs_P_dy)).xy * sizeViewport;

  /* Area of the square in pixels. */
  float area = length(cross(vec3(ss_P_dx - ss_P, 0.0), vec3(ss_P_dy - ss_P, 0.0)));
  /* TODO(fclem): Coloring for axes. */
  /* TODO(fclem): Scale depending on UI scale. */
  finalColor = vec4(colorGrid.rgb, smoothstep(square(8.0), square(32.0), area));

  edgePos = edgeStart = ss_P;

  gl_Position = hs_P;
}
