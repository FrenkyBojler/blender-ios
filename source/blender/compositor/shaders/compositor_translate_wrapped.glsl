/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

// This does not appear to be used currently

#include "gpu_shader_compositor_sampleRect.glsl"

void main()
{
  int2 texel = int2(gl_GlobalInvocationID.xy);

  float2 uv = (float2(texel) - translation);
  float2 wh = float2(1.0f);

  imageStore(output_img, texel, sampleRect(uv, wh));
}
