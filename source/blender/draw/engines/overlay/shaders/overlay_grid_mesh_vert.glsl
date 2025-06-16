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

float approximate_grid_cell_screen_size(float dist_to_cam, float view_angle)
{
  float3 vs_P = float3(0.0, unit_scale, -dist_to_cam);
  float3 ss_P = drw_point_view_to_screen(vs_P);
  return (ss_P.y - 0.5) * uniform_buf.size_viewport.y * sqrt(abs(view_angle));
}

float4 get_homogenous_space_grid_point(
    int x, int y, out float dist_to_cam, out float z_to_cam, out float view_angle)
{
  float3 ls_P = float3(x, y, 0.0) * unit_scale;

  if (axis == 3) {
    ls_P = ls_P.zzx;
  }

  float snap_to = next_divider * unit_scale;
  /* Round to grid increment. */
  float3 camera_P = drw_view_position();
  if (axis > 0) {
    /* TODO(fclem): Slide on the axis only. */
    ls_P.xy -= camera_P.xy;
  }
  else {
    ls_P.xy -= fract(camera_P.xy / snap_to) * snap_to;
  }
  ls_P.z -= camera_P.z;

  dist_to_cam = length(ls_P);

  /* ls_P is already centered around the camera. */
  float3 V = drw_view_is_perspective() ? ls_P * safe_rcp(dist_to_cam) : drw_view_forward();
  view_angle = V.z;
  if (axis == 3) {
    /* Don't fade. */
    view_angle = 1.0;
  }

  /* Do not use matrix translation as it degrades precision. */
  float3 vs_P = drw_normal_world_to_view(ls_P);

  z_to_cam = abs(vs_P.z);

  return drw_point_view_to_homogenous(vs_P);
}

void main()
{
  int x = int(uint(gl_VertexID) >> 16u) - 0x7FFF;
  int y = int(uint(gl_VertexID) & (~0x0u >> 16u)) - 0x7FFF;

  float dist_to_cam, z_to_cam, view_angle;
  float4 hs_P = get_homogenous_space_grid_point(x, y, dist_to_cam, z_to_cam, view_angle);

  /* Convert to screen position [0..sizeVp]. */
  float2 ss_P = drw_ndc_to_screen(drw_perspective_divide(hs_P)).xy * uniform_buf.size_viewport;

  if (axis == 1) {
    finalColor = uniform_buf.colors.grid_axis_x;
  }
  else if (axis == 2) {
    finalColor = uniform_buf.colors.grid_axis_y;
  }
  else if (axis == 3) {
    finalColor = uniform_buf.colors.grid_axis_z;
  }
  else {
    /* Area of the projected tile in pixels. */
    float size = approximate_grid_cell_screen_size(dist_to_cam, view_angle) /
                 uniform_buf.sizes.pixel;
    float mix_fade = smoothstep(2.0, 25.0, size);
    /* TODO(fclem): Adjust with relative density with level N-2. */
    float mix_highlight = smoothstep(20.0, 300.0, size);
    finalColor = mix(uniform_buf.colors.grid, uniform_buf.colors.grid_emphasis, mix_highlight);
    finalColor.a *= mix_fade;
  }

  /* Angle fading. */
  finalColor.a *= 1.0 - square(square(1.0 - abs(view_angle)));

  /* Distance fading. */
  finalColor.a *= smoothstep(far_clip, far_clip * 0.5f, z_to_cam);

  edgePos = edgeStart = ss_P;

  gl_Position = hs_P;
  /* Depth offset to avoid Z fighting with other overlays and coplanar surfaces. */
  gl_Position.z += 2.4e-5;
}
