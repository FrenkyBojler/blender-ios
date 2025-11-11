/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "GPU_shader_shared.hh"
#endif

#include "GPU_xr_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(gpu_shader_xr_raycast)

FRAGMENT_OUT(0, float4, fragColor)
DEFINE_VALUE("XR_TELEPORTATION_ARC_SAMPLES", STRINGIFY(XR_TELEPORTATION_ARC_SAMPLES))

PUSH_CONSTANT_ARRAY(float4, control_points, XR_TELEPORTATION_ARC_CONTROL_POINTS)
PUSH_CONSTANT(float4x4, ModelViewProjectionMatrix)
PUSH_CONSTANT(float4, color)
PUSH_CONSTANT(float3, right_vector)
PUSH_CONSTANT(float, line_width)
PUSH_CONSTANT(int, end_point_idx)

VERTEX_SOURCE("gpu_shader_xr_raycast_vert.glsl")
FRAGMENT_SOURCE("gpu_shader_uniform_color_frag.glsl")
ADDITIONAL_INFO(gpu_srgb_to_framebuffer_space)
DO_STATIC_COMPILATION()

GPU_SHADER_CREATE_END()
