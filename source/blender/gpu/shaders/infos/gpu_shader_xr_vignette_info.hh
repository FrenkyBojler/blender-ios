/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"

#  include "GPU_shader_shared.hh"

#  include "overlay_shader_shared.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(gpu_shader_xr_vignette)
TYPEDEF_SOURCE("overlay_shader_shared.hh")
VERTEX_IN(0, float2, pos)
FRAGMENT_OUT(0, float4, fragColor)
PUSH_CONSTANT(float4x4, ModelViewProjectionMatrix)
PUSH_CONSTANT(float4, background)
PUSH_CONSTANT(float4, background_gradient)
PUSH_CONSTANT(float2, viewportSize)
PUSH_CONSTANT(float, aperture)
PUSH_CONSTANT(float, falloff)
PUSH_CONSTANT(int, background_type)
VERTEX_SOURCE("gpu_shader_2D_vert.glsl")
FRAGMENT_SOURCE("gpu_shader_xr_vignette_frag.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
