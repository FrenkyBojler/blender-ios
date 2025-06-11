/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_signed_distance_field_compute_boundary)
LOCAL_GROUP_SIZE(16, 16)
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, GPU_RG16I, write, iimage2D, boundary_img)
COMPUTE_SOURCE("compositor_signed_distance_field_compute_boundary.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_signed_distance_field_compute_distance)
LOCAL_GROUP_SIZE(16, 16)
SAMPLER(0, sampler2D, input_tx)
SAMPLER(1, isampler2D, flooded_boundary_tx)
IMAGE(0, GPU_R16F, write, image2D, distance_img)
COMPUTE_SOURCE("compositor_signed_distance_field_compute_distance.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
