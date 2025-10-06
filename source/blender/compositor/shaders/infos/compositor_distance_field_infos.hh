/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_distance_field_calculate_edges)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(bool, include_diagonal)
SAMPLER(0, sampler2D, mask_tx)
IMAGE(0, SINT_16_16, write, iimage2D, edges_img)
COMPUTE_SOURCE("compositor_distance_field_calculate_edges.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_distance_field_calculate_distance)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(bool, should_normalize)
PUSH_CONSTANT(bool, is_signed)
SAMPLER(0, sampler2D, mask_tx)
SAMPLER(1, isampler2D, positions_tx)
IMAGE(0, SFLOAT_16, write, image2D, dist_img)
COMPUTE_SOURCE("compositor_distance_field_calculate_distance.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()