/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_math_vector_reduce_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"
#include "overlay_common_lib.glsl"

#define linearstep(p0, p1, v) (clamp(((v) - (p0)) / abs((p1) - (p0)), 0.0f, 1.0f))

/* TODO(not_mark): extract to infos */
#define GRID_LEVELS_DRAW 3

void main()
{
  /* Fragment color. */
  {
    /* Base color is a mix of [grid, grid_emphasis] by vertex alpha, which incorporates level
     * and subpixel fades only. */
    out_color = mix(theme.colors.grid, theme.colors.grid_emphasis, local_alpha);
    out_color.a *= local_alpha;

    /* Primary axis colors. */
    if (flag_test(grid_flag, SHOW_AXIS_X) && reduce_max(abs(local_pos.yz)) < 1e-4f) {
      out_color.rgb = theme.colors.grid_axis_x.rgb;
      out_color.a = max(out_color.a, theme.colors.grid_axis_x.a);
    }
    if (flag_test(grid_flag, SHOW_AXIS_Y) && reduce_max(abs(local_pos.xz)) < 1e-4f) {
      out_color.rgb = theme.colors.grid_axis_y.rgb;
      out_color.a = max(out_color.a, theme.colors.grid_axis_y.a);
    }
    if (flag_test(grid_flag, SHOW_AXIS_Z) && reduce_max(abs(local_pos.xy)) < 1e-4f) {
      out_color.rgb = theme.colors.grid_axis_z.rgb;
      out_color.a = max(out_color.a, theme.colors.grid_axis_z.a);
    }
  }

  /* Fragment alpha. */
  {
    if (drw_view_is_perspective()) {
      /* Fade at edge of grid level. */
      float length_fade = 1.f - min(1.f, dot(local_coord, local_coord));
      out_color.a *= pow2f(length_fade);

      /* Compute normalized view vector. */
      float3 V = drw_view_position() - local_pos;
      float dist = length(V);
      V /= dist;

      /* Add fade at steep angles for contents of the floor plane. */
      if (!flag_test(grid_flag, DRAW_AXIS_Z)) {
        out_color.a *= 1.0f - pow3f(1.0f - abs(V.z));
      }

      /* Add fade towards clip distance. */
      out_color.a *= 1.0f -
                     smoothstep(0.0f, 0.5f * grid_buf.distance, dist - 0.5f * grid_buf.distance);
    }
    else {
      /* Fade at edge of grid level in orthographic, in case of rather small units. */
      if (!flag_test(grid_flag, PLANE_IMAGE)) {
        float length_fade = 1.f - min(1.f, dot(local_coord, local_coord));
        out_color.a *= pow2f(length_fade);
      }

      /* Avoid fading in +Z direction in camera view (see #70193).
       * This is reproduced from the 5.0 grid line-for-line. */
      float dist = gl_FragCoord.z * 2.0f - 1.0f;
      dist = flag_test(grid_flag, GRID_CAMERA) ? clamp(dist, 0.0f, 1.0f) : abs(dist);
      out_color.a *= (1.0f - smoothstep(0.0f, 0.5f, dist - 0.5f));

      if (flag_test(grid_flag, PLANE_XY)) {
        float angle = 1.0f - abs(drw_view().viewinv[2].z);
        out_color.a *= (1.f - pow3f(angle));
      }
    }
  }

  /* Depth test/fade. */
  {
    /* Perform depth texture lookup */
    float2 uv = gl_FragCoord.xy / float2(textureSize(depth_tx, 0));
    float scene_depth = texture(depth_tx, uv, 0).r;

    /* Perform depth-infront texture lookup. If this value is set, an object is treated
     * as if it is on the near-plane, always occluding the grid. */
    float scene_depth_infront = texture(depth_infront_tx, uv, 0).r;
    if (scene_depth_infront != 1.0f) {
      scene_depth = 0.0f;
    }

    /* Compute grid depth. As in 5.0, a small bias places the grid below
     * a mesh with the same depth. */
    float grid_depth = gl_FragCoord.z + 4.8e-7f * float(GRID_LEVELS_DRAW - 1 - local_level);
    gl_FragDepth = grid_depth;

    /* Soft depth-test as in 5.0, progressively alpha the grid below occluders to
     * avoid popping and flickering. This gives the grid a see-through appearance. */
    float bias = max(gpu_fwidth(gl_FragCoord.z), 2.4e-7f);
    out_color.a *= linearstep(grid_depth, grid_depth + bias, scene_depth);
  }

  /* Output for viewport antialiasing. */
  if (out_color.a != 0.0f) {
    line_output = pack_line_data(gl_FragCoord.xy, edge_start, edge_pos);
  }

  float3 cols[3] = {
    float3(1, 0, 0),
    float3(0, 1, 0),
    float3(0, 0, 1)
  };
  out_color.rgb = cols[local_level];
}
