/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_3D_image_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_3D_image)

#include "gpu_shader_colorspace_lib.glsl"

void main()
{
  fragColor = texture(image, texCoord_interp);
  if (is_scene_referred_linear_image) {
    fragColor = blender_scene_linear_to_framebuffer_space(fragColor);
  }
  else {
    fragColor = blender_srgb_to_framebuffer_space(fragColor);
  }
}
