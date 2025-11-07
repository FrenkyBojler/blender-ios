/* SPDX-FileCopyrightText: 2017-2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/overlay_grid_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(overlay_gridrework_next)

#include "draw_view_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

#define GRID_DEPTH_BIAS 4.8e-7f
#define linearstep(p0, p1, v) (clamp(((v) - (p0)) / abs((p1) - (p0)), 0.0f, 1.0f))

void main()
{
  /* Compute normalized view vector. */
  float3 P = local_pos;
  float3 V = drw_view_position() - P;
  float dist = length(V);
  V /= dist;

  out_color = theme.colors.grid_emphasis;

  /* Output color. */
  {
    
  }

  /* Output alpha. */
  {
    /* Modulate perspective fade */
    /* TODO: Have someone sanity check this, I'm uncertain if it is working (~Mark). */
    float2 grid_fwidth_xy = fwidth(proj_xy.xy);
    float grid_fwidth = 1.f - min(1.f, max(grid_fwidth_xy.x, grid_fwidth_xy.y));
    grid_fwidth *= grid_fwidth;
    out_color.a *= grid_fwidth;

    /* Add fade at steep angles. */
    float angle = V.z;
    angle = 1.0f - abs(angle);
    angle *= angle;
    out_color.a *= (1.f - angle * angle);

    /* Add stepped fade towards clip distance. */
    out_color.a *= 1.0f - smoothstep(0.0f, grid_buf.distance, dist - grid_buf.distance);

    /* Add fade towards edge of current level's edge. */
    float length_fade = 1.f - min(1.f, dot(frag_xy, frag_xy));
    length_fade *= length_fade;
    out_color.a *= length_fade * length_fade;

    /* Add fade at level switch. This is computed in the vertex stage. */
    out_color.a *= frag_level;
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

    /* Compute grid depth. As in old grid; a small bias places the grid below
     * a mesh with the same depth. */
     float grid_depth = gl_FragCoord.z + GRID_DEPTH_BIAS;

    /* Manipulate alpha with scene depth */
    // if (scene_depth != 1.0f) {
    //   alpha = 0.0f;
    // }
    
    /* Soft depth-test; progressively alpha the grid below occluders to
     * avoid popping visuals and flickering. This gives the grid a slight
     * see-through appearance. */
    float bias = max(gpu_fwidth(gl_FragCoord.z), 2.4e-7f);
    out_color.a *= linearstep(grid_depth, grid_depth + bias, scene_depth);
  }
}