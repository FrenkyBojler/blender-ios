/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#ifdef GLSL_CPP_STUBS
#  define SAMPLER_FUNCTION texture
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_masked_maximum_shared)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(bool, output_image_should_compute)
PUSH_CONSTANT(bool, output_chosen_image_pixel_should_compute)
PUSH_CONSTANT(bool, output_chosen_mask_pixel_should_compute)
PUSH_CONSTANT(int2, domain_data_size)
PUSH_CONSTANT(int2, input_mask_domain_data_size)
PUSH_CONSTANT(bool, keep_seamless)
SAMPLER(0, sampler2D, input_image_tx)
SAMPLER(1, sampler2D, input_mask_tx)
SAMPLER(2, sampler2D, input_mask_size_tx)
SAMPLER(3, sampler2D, input_rotation_tx)
SAMPLER(4, sampler2D, input_translation_tx)
SAMPLER(5, sampler2D, input_rounding_tx)
SAMPLER(6, sampler2D, input_hardness_tx)
SAMPLER(7, sampler2D, input_ellipse_height_tx)
SAMPLER(8, sampler2D, input_ellipse_width_tx)
SAMPLER(9, sampler2D, input_inflection_midpoint_tx)
SAMPLER(10, sampler2D, input_value_boundary_tx)
IMAGE(0, SFLOAT_16, write, image2D, output_image_img)
IMAGE(1, SFLOAT_16_16, write, image2D, output_chosen_image_pixel_img)
IMAGE(2, SFLOAT_16_16, write, image2D, output_chosen_mask_pixel_img)
COMPUTE_SOURCE("compositor_masked_maximum.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_masked_maximum)
ADDITIONAL_INFO(compositor_masked_maximum_shared)
DEFINE_VALUE("SAMPLER_FUNCTION", "texture")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_masked_maximum_bicubic)
ADDITIONAL_INFO(compositor_masked_maximum_shared)
DEFINE_VALUE("SAMPLER_FUNCTION", "texture_bicubic")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
