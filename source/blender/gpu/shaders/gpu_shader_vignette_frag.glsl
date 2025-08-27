/* SPDX-FileCopyrightText: 2017-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_vignette_info.hh"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_vignette)

void main()
{
  float2 uv = gl_FragCoord.xy / viewportSize;
  float dist = length(uv - float2(0.5));

  // Create vignette factor using smoothstep
  float vignette = smoothstep(aperture, aperture + falloff, dist);

  // Output the vignette color with fade applied
  fragColor = vec4(color.rgb, color.a * vignette);
}
