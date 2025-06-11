/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  const int2 texel = int2(gl_GlobalInvocationID.xy);

  const bool is_inside_mask = texture_load(input_tx, texel).x != 0.0f;
  const int2 closest_boundary_texel = texture_load(flooded_boundary_tx, texel).xy;
  const float distance_to_boundary = distance(float2(texel), float2(closest_boundary_texel));
  const float signed_distance = is_inside_mask ? -distance_to_boundary : distance_to_boundary;

  imageStore(distance_img, texel, float4(signed_distance));
}
