/* SPDX-FileCopyrightText: 2016-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/gpu_shader_fullscreen_info.hh"

FRAGMENT_SHADER_CREATE_INFO(gpu_shader_framebuffer_uniform_color_frag)

void main()
{
  fragColor0 = color;
  fragColor1 = color;
  fragColor2 = color;
  fragColor3 = color;
  fragColor4 = color;
  fragColor5 = color;
  fragColor6 = color;
  fragColor7 = color;
}
