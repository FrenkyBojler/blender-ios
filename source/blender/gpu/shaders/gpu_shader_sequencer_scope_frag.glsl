/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_sequencer_info.hh"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_sequencer_scope)

/* Very similar to gpu_shader_point_uniform_color_aa_frag, except no discard,
 * use color from avarying and no sRGB conversions. */

void main()
{
  float dist = length(gl_PointCoord - float2(0.5f));

  fragColor = finalColor;
  fragColor.a = mix(finalColor.a, 0.0f, smoothstep(radii[1], radii[0], dist));
}
