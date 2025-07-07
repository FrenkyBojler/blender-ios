/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_INTERFACE_INFO(gpu_fullscreen_iface)
SMOOTH(float2, screen_uv)
GPU_SHADER_INTERFACE_END()

GPU_SHADER_CREATE_INFO(gpu_fullscreen)
VERTEX_OUT(gpu_fullscreen_iface)
VERTEX_SOURCE("gpu_shader_fullscreen_vert.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(gpu_shader_fullscreen_uniform_color)
VERTEX_OUT(gpu_fullscreen_iface)
FRAGMENT_OUT(0, float4, fragColor0)
FRAGMENT_OUT(1, float4, fragColor1)
FRAGMENT_OUT(2, float4, fragColor2)
FRAGMENT_OUT(3, float4, fragColor3)
FRAGMENT_OUT(4, float4, fragColor4)
FRAGMENT_OUT(5, float4, fragColor5)
FRAGMENT_OUT(6, float4, fragColor6)
FRAGMENT_OUT(7, float4, fragColor7)
PUSH_CONSTANT(float4, color)
VERTEX_SOURCE("gpu_shader_fullscreen_vert.glsl")
FRAGMENT_SOURCE("gpu_shader_framebuffer_uniform_color_frag.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
