/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_compat.hh"

#include "gpu_shader_create_info.hh"

/* Runtime create info. */
GPU_SHADER_CREATE_INFO(gpu_framebuffer_layer_viewport_test)
FRAGMENT_OUT(0, int2, out_value)
GPU_SHADER_CREATE_END()

#ifdef GPU_VERTEX_SHADER
VERTEX_SHADER_CREATE_INFO(gpu_framebuffer_layer_viewport_test)

void main()
{
  /* Full-screen triangle. */
  int v = gl_VertexID % 3;
  float x = -1.0f + float((v & 1) << 2);
  float y = -1.0f + float((v & 2) << 1);
  /* NOTE: Make it cover more than one viewport to test default scissors. */
  gl_Position = float4(x * 2.0f, y * 2.0f, 1.0f, 1.0f);

  int index = gl_VertexID / 3;
  gpu_ViewportIndex = index % 16;
  gpu_Layer = index / 16;
}
#endif

#ifdef GPU_FRAGMENT_SHADER
FRAGMENT_SHADER_CREATE_INFO(gpu_framebuffer_layer_viewport_test)

void main()
{
  out_value = int2(gpu_Layer, gpu_ViewportIndex);
}
#endif
