/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_common_color_utils.glsl"
#include "infos/gpu_shader_sequencer_info.hh"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_sequencer_zebra)

void main()
{
  float4 color = texture(image, texCoord_interp);
  if (img_premultiplied) {
    color_alpha_unpremultiply(color, color);
  }

  fragColor = float4(0.0);
  int phase = int(mod((gl_FragCoord.x + gl_FragCoord.y), 6.0));
  if (any(greaterThan(color.rgb, float3(zebra_limit)))) {
    if (phase == 4) {
      fragColor = float4(0.0, 0.0, 0.0, 0.85);
    }
    else if (phase >= 3) {
      fragColor = float4(1.0, 0.0, 0.5, 0.95);
    }
  }
}
