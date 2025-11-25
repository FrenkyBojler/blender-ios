/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_grid_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_vector_reduce_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "overlay_common_lib.glsl"

void main()
{
  /* Base color is a mix of [grid, emphasis] by vertex alpha, which incorporates level
   * and subpixel fades only. */
  out_color = mix(theme.colors.grid, theme.colors.grid_emphasis, vertex_out.alpha);
  out_color.a *= vertex_out.alpha;

  /* Primary axes colors override base color. */
  if (flag_test(grid_flag, AXIS_X) && reduce_max(abs(vertex_out.pos.yz)) < 1e-4f) {
    out_color.rgb = theme.colors.grid_axis_x.rgb;
    out_color.a = max(out_color.a, theme.colors.grid_axis_x.a);
  }
  else if (flag_test(grid_flag, AXIS_Y) && reduce_max(abs(vertex_out.pos.xz)) < 1e-4f) {
    out_color.rgb = theme.colors.grid_axis_y.rgb;
    out_color.a = max(out_color.a, theme.colors.grid_axis_y.a);
  }
  else if (flag_test(grid_flag, AXIS_Z) && reduce_max(abs(vertex_out.pos.xy)) < 1e-4f) {
    out_color.rgb = theme.colors.grid_axis_z.rgb;
    out_color.a = max(out_color.a, theme.colors.grid_axis_z.a);
  }

  /* Fragment alpha. */
  if (drw_view_is_perspective()) {
    /* Fade at edge of grid level. */
    float length_fade = 1.0f - min(1.0f, dot(vertex_out.coord, vertex_out.coord));
    out_color.a *= pow2f(length_fade);

    /* Compute normalized view vector. */
    float3 V = drw_view_position() - vertex_out.pos;
    float dist = length(V);
    V /= dist;

    /* Add fade at steep angles for contents of the floor plane. */
    if (vertex_out.pos.z == 0.0f) {
      out_color.a *= 1.0f - pow3f(1.0f - abs(V.z));
    }

    /* Add fade towards camera clip plane. */
    float far_clip = -drw_view_far();
    out_color.a *= 1.0f - smoothstep(0.0f, 0.5f * far_clip, dist - 0.5f * far_clip);
  }
  else {
    /* Fade at edge of grid level in orthographic, in case of rather small units. */
    if (!flag_test(grid_flag, PLANE_IMAGE)) {
      float length_fade = 1.0f - min(1.0f, dot(vertex_out.coord, vertex_out.coord));
      out_color.a *= pow2f(length_fade);
    }

    /* Avoid fading in +Z direction in camera view (see #70193).
     * This is reproduced from the 5.0 grid line-for-line. */
    float dist = gl_FragCoord.z * 2.0f - 1.0f;
    dist = flag_test(grid_flag, GRID_CAMERA) ? clamp(dist, 0.0f, 1.0f) : abs(dist);
    out_color.a *= (1.0f - smoothstep(0.0f, 0.5f, dist - 0.5f));

    if (flag_test(grid_flag, PLANE_XY)) {
      float angle = 1.0f - abs(drw_view().viewinv[2].z);
      out_color.a *= (1.0f - pow3f(angle));
    }
  }

  /* Viewport antialiasing output. */
  if (out_color.a != 0.0f) {
    line_output = pack_line_data(gl_FragCoord.xy, vertex_out.edge_start, vertex_out.edge_pos);
  }
}
