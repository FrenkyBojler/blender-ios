/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_morphological_distance_jump_flooding)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(bool, is_dilate)
PUSH_CONSTANT(bool, is_initial_pass)
PUSH_CONSTANT(int, radius)
PUSH_CONSTANT(int, step_size)
SAMPLER(0, sampler2D, input_tx)
SAMPLER(1, isampler2D, input_jump_flooding_table_tx)
IMAGE(0, SINT_16_16, write, iimage2D, output_jump_flooding_table_img)
IMAGE(1, SFLOAT_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_morphological_distance_jump_flooding.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
