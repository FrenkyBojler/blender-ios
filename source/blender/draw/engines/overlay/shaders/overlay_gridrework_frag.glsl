/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_math_base_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

#define linearstep(p0, p1, v) (clamp(((v) - (p0)) / abs((p1) - (p0)), 0.0f, 1.0f))

void main()
{
  /* Compute normalized view vector. */
  float3 P = local_pos;
  float3 V = drw_view_position() - P;
  float dist = length(V);
  V /= dist;

  /* Output color is specified by theme. */
  out_color = theme.colors.grid;

  /* Output alpha. */
  {
    /* Add fade from vertex stage. */
    out_color.a *= local_alpha;

    /* Add fade at edge of grid level. */
    float length_fade = 1.f - min(1.f, dot(local_coord, local_coord));
    out_color.a *= pow2f(length_fade);

    if (drw_view_is_perspective()) {
      /* Add fade at steep angles. */
      float angle = 1.0f - abs(V.z);
      out_color.a *= (1.f - pow3f(angle));

      /* Add fade towards clip distance. */
      out_color.a *= 1.0f -
                     smoothstep(0.0f, 0.5f * grid_buf.distance, dist - 0.5f * grid_buf.distance);
    }
    else {
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

  /* Depth testing. */
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
    float grid_depth = gl_FragCoord.z + 4.8e-7f;

    /* Soft depth-test as in 5.0, progressively alpha the grid below occluders to
     * avoid popping and flickering. This gives the grid a see-through appearance. */
    float bias = max(gpu_fwidth(gl_FragCoord.z), 2.4e-7f);
    out_color.a *= linearstep(grid_depth, grid_depth + bias, scene_depth);
  }
}
