/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compositor_texture_utilities.glsl"
#include "gpu_shader_compositor_sampleRect.glsl"

float2 dx(sampler2D image, int2 texel) {
  return (texture_load(image, texel + int2(1,0)).xy - texture_load(image, texel - int2(1,0)).xy) / 2;
}

float2 dy(sampler2D image, int2 texel) {
  return (texture_load(image, texel + int2(0,1)).xy - texture_load(image, texel - int2(0,1)).xy) / 2;
}

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);
  int2 input_size = texture_size(input_tx);

  float2 scale = float2(texture_load(x_scale_tx, texel).x, texture_load(y_scale_tx, texel).x);
  float2 uv = float2(texel) - texture_load(displacement_tx, texel).xy * scale;

  // derivative of scale is ignored, assumed to be close to zero
  float2 wh = clamp(hypot2(
                      float2(1,0) - dx(displacement_tx, texel) * scale,
                      float2(0,1) - dy(displacement_tx, texel) * scale
                    ), 1.0f, 63.0f);

  imageStore(output_img, texel, sampleRect(uv, wh));
}
