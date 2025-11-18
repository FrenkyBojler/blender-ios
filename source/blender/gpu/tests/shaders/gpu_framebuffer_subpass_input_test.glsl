/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compat.hh"

#include "gpu_shader_create_info.hh"

/* Runtime create info. */
GPU_SHADER_CREATE_INFO(gpu_framebuffer_subpass_input_test)
FRAGMENT_OUT(0, int, out_value)
GPU_SHADER_CREATE_END()

#ifdef GPU_VERTEX_SHADER
VERTEX_SHADER_CREATE_INFO(gpu_framebuffer_subpass_input_test)

void main()
{
  /* Full-screen triangle. */
  int v = gl_VertexID % 3;
  float x = -1.0f + float((v & 1) << 2);
  float y = -1.0f + float((v & 2) << 1);
  gl_Position = float4(x, y, 1.0f, 1.0f);
}
#endif

#ifdef GPU_FRAGMENT_SHADER
FRAGMENT_SHADER_CREATE_INFO(gpu_framebuffer_subpass_input_test)

#  ifdef WRITE
void main()
{
  out_value = 0xDEADBEEF;
}
#  endif

#  ifdef READ
void main()
{
  out_value = in_value + 495;
}
#  endif

#endif
