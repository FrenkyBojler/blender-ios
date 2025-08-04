/* SPDX-FileCopyrightText: 2017-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_vignette_info.hh"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_vignette)

void main()
{  
  float dist = length(texCoord_interp - vec2(0.5));

  // Create vignette factor using smoothstep
  float vignette = smoothstep(aperture, aperture + falloff, dist);

  // Output the vignette color with fade applied
  fragColor = vec4(color.rgb, color.a * vignette);
}
