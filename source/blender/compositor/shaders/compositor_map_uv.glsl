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

  float4 uva = texture_load(uv_tx, texel);
  /* The UV texture is assumed to contain an alpha channel as its third channel, since the UV
   * coordinates might be defined in only a subset area of the UV texture as mentioned. In that
   * case, the alpha is typically opaque at the subset area and transparent everywhere else, and
   * alpha pre-multiplication is then performed. This format of having an alpha channel in the UV
   * coordinates is the format used by UV passes in render engines, hence the mentioned logic. */
  if (uva.z <= 0) {
    imageStore(output_img, texel, float4(0));
  } else {
    float2 scale = float2(imageSize(output_img));
    float2 uv = uva.xy * scale - 0.5f;
    float2 wh = clamp(hypot2(dx(uv_tx, texel), dy(uv_tx, texel)) * scale, 1.0f, 63.0f);
    imageStore(output_img, texel, sampleRect(uv, wh) * uva.z);
  }
}
