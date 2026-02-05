/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpencil_infos.hh"

#include "draw_view_lib.glsl"

FRAGMENT_SHADER_CREATE_INFO(gpencil_depth_pass_merge)

void main()
{
  const int2 texel = int2(gl_FragCoord.xy);
  const float2 normalized_coordinates = gl_FragCoord.xy / float2(textureSize(depth_buf, 0));

  const float depth_3d = textureLod(depth_buf, normalized_coordinates, 0).x;
  const float depth_2d = depth_3d != 0.0f ? gl_FragCoord.z : 1.0f;
  const float depth = stroke_order3d ? depth_3d : depth_2d;
  const float view_depth = -drw_depth_screen_to_view(depth);

  const float scene_depth = imageLoad(depth_pass_img, texel).x;

  const float combined_depth = min(scene_depth, view_depth);
  imageStore(depth_pass_img, texel, float4(combined_depth));
}
