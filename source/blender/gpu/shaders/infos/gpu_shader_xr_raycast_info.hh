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
#endif

#include "gpu_shader_create_info.hh"

/* TODO(fclem): Share with C code. */
#define XR_MAX_RAYCASTS 8

GPU_SHADER_CREATE_INFO(gpu_shader_xr_raycast)
DEFINE_VALUE("XR_MAX_RAYCASTS", STRINGIFY(XR_MAX_RAYCASTS))
FRAGMENT_OUT(0, float4, fragColor)
PUSH_CONSTANT_ARRAY(float4, controlPoints, XR_MAX_RAYCASTS + 1)
PUSH_CONSTANT(float4x4, ModelViewProjectionMatrix)
PUSH_CONSTANT(float4, color)
PUSH_CONSTANT(float3, rightVector)
PUSH_CONSTANT(float, width)
PUSH_CONSTANT(int, controlPointCount)
PUSH_CONSTANT(int, samplesPerSegment)
VERTEX_SOURCE("gpu_shader_xr_raycast_vert.glsl")
FRAGMENT_SOURCE("gpu_shader_uniform_color_frag.glsl")
ADDITIONAL_INFO(gpu_srgb_to_framebuffer_space)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
