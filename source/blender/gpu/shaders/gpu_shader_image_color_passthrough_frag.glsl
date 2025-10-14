/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_3D_image_infos.hh"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_3D_image_passthrough)

void main()
{
  fragColor = texture(image, texCoord_interp);
  /* NOTE(fclem): This expects the image color space matches the framebuffer colorspace. */
}
