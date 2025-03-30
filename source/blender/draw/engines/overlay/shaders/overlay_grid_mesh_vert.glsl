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

vec4 get_homogenous_space_grid_point(int x, int y, out float dist_to_cam, out float view_angle)
{
  vec3 ls_P = vec3(x, y, 0.0) * unit_scale;

  if (axis == 3) {
    ls_P = ls_P.zzx;
  }

  float snap_to = next_divider * unit_scale;
  /* Round to grid increment. */
  vec3 camera_P = drw_view_position();
  if (axis > 0) {
    /* TODO(fclem): Slide on the axis only. */
    ls_P.xy -= camera_P.xy;
  }
  else {
    ls_P.xy -= fract(camera_P.xy / snap_to) * snap_to;
  }
  ls_P.z -= camera_P.z;

  /* ls_P is already centered around the camera. */
  vec3 V = drw_view_is_perspective() ? normalize(ls_P) : drw_view_forward();
  view_angle = V.z;
  if (axis == 3) {
    /* Don't fade. */
    view_angle = 1.0;
  }

  /* Do not use matrix translation as it degrades precision. */
  vec3 vs_P = drw_normal_world_to_view(ls_P);

  dist_to_cam = abs(vs_P.z);

  return drw_point_view_to_homogenous(vs_P);
}

void main()
{
  int x = int(uint(gl_VertexID) >> 16u) - 0x7FFF;
  int y = int(uint(gl_VertexID) & (~0x0u >> 16u)) - 0x7FFF;

  float dist_to_cam, view_angle;
  /* Adjacent points used to get screen space grid density. */
  vec4 hs_P_dx = get_homogenous_space_grid_point(x + 1, y, dist_to_cam, view_angle);
  vec4 hs_P_dy = get_homogenous_space_grid_point(x, y + 1, dist_to_cam, view_angle);
  vec4 hs_P = get_homogenous_space_grid_point(x, y, dist_to_cam, view_angle);

  /* Convert to screen position [0..sizeVp]. */
  vec2 ss_P = drw_ndc_to_screen(drw_perspective_divide(hs_P)).xy * sizeViewport;
  vec2 ss_P_dx = drw_ndc_to_screen(drw_perspective_divide(hs_P_dx)).xy * sizeViewport;
  vec2 ss_P_dy = drw_ndc_to_screen(drw_perspective_divide(hs_P_dy)).xy * sizeViewport;

  if (axis == 1) {
    finalColor = colorGridAxisX;
  }
  else if (axis == 2) {
    finalColor = colorGridAxisY;
  }
  else if (axis == 3) {
    finalColor = colorGridAxisZ;
  }
  else {
    /* TODO(fclem): Scale area depending on UI scale. */
    /* Area of the projected tile in pixels. */
    float area = length(cross(vec3(ss_P_dx - ss_P, 0.0), vec3(ss_P_dy - ss_P, 0.0)));
    float fac = sqrt(area);
    float mix_fade = smoothstep(1.0, 30.0, fac);
    /* TODO(fclem): Adjust with relative density with level N-2. */
    float mix_highlight = smoothstep(500.0, 1000.0, fac);
    finalColor = mix(colorGrid, colorGridEmphasis, mix_highlight);
    finalColor.a *= mix_fade;
  }

  /*  Angle fading. */
  finalColor.a *= 1.0 - square(square(1.0 - abs(view_angle)));

  /* Distance fading. */
  finalColor.a *= smoothstep(far_clip, far_clip * 0.5f, dist_to_cam);

  edgePos = edgeStart = ss_P;

  gl_Position = hs_P;
  /* Depth offset to avoid Z fighting with other overlays and coplanar surfaces. */
  gl_Position.z += 2.4e-5;
}
