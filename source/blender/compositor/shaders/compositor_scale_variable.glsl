/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_compositor_sampleRect.glsl"

float dx(sampler2D image, int2 texel) {
  return (texture_load(image, texel + int2(1,0)).x - texture_load(image, texel - int2(1,0)).x) / 2;
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  float2 center = float2(texture_size(input_tx)) / 2;

  float2 uv = float2(texel) + 0.5f - center; // use center coordinates
  float2 scale = 1 / max(float2(texture_load(x_scale_tx, texel).x, texture_load(y_scale_tx, texel).x), 0.0001f);
  float2 dscale = -float2(dx(x_scale_tx, texel), dx(y_scale_tx, texel)) * scale * scale; // derivative of scale
  float2 wh = abs(uv * dscale + scale); // derivative of uv * scale
  uv = uv * scale + center;

  imageStore(output_img, texel, sampleRect(uv, wh));
}
