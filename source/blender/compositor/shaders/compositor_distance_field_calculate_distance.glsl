/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  int2 nearest_pos = texture_load(positions_tx, texel).xy;

  float dist = distance(texel, nearest_pos);

  if (should_normalize)
  {
    int2 domain = texture_size(positions_tx);
    float imageMagnitude = float(max(domain.x, domain.y));
  
    dist = dist / imageMagnitude;
  }

  if (is_signed && texture_load(mask_tx, texel).x != 0.0f)
  {
    dist = dist * -1;
  }

  imageStore(dist_img, texel, float4(dist));
}