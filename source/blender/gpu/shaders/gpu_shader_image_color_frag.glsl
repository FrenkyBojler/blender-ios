/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_3D_image_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_3D_image_color_scene_linear)

#include "gpu_shader_colorspace_lib.glsl"

void main()
{
  fragColor = texture(image, texCoord_interp) * color;
  if (is_scene_linear_image) {
    /* TODO(fclem): Add color transform here to convert scene linear to Rec.709 linear. */
    fragColor = blender_linear_to_framebuffer_space(fragColor);
  }
  else {
    fragColor = blender_srgb_to_framebuffer_space(fragColor);
  }
}
