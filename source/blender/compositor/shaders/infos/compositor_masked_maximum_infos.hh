/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_masked_maximum)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(int2, domain_data_size)
PUSH_CONSTANT(bool, keep_seamless)
SAMPLER(0, sampler2D, input_image_tx)
SAMPLER(1, sampler2D, input_size_tx)
SAMPLER(2, sampler2D, input_roundness_tx)
SAMPLER(3, sampler2D, input_falloff_width_tx)
SAMPLER(4, sampler2D, input_falloff_boundary_value_tx)
SAMPLER(5, sampler2D, input_ellipse_height_tx)
SAMPLER(6, sampler2D, input_ellipse_width_tx)
SAMPLER(7, sampler2D, input_inflection_midpoint_tx)
SAMPLER(8, sampler2D, input_rotation_tx)
SAMPLER(9, sampler2D, input_translation_tx)
IMAGE(0, SFLOAT_16, write, image2D, output_image_img)
COMPUTE_SOURCE("compositor_masked_maximum.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
