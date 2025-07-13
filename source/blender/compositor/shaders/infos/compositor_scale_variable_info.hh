/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_scale_variable)
LOCAL_GROUP_SIZE(16, 16)
SAMPLER(0, sampler2D, input_tx)
SAMPLER(1, sampler2D, x_scale_tx)
SAMPLER(2, sampler2D, y_scale_tx)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, output_img)
COMPUTE_SOURCE("compositor_scale_variable.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_scale_variable_nearest)
ADDITIONAL_INFO(compositor_scale_variable)
DEFINE_VALUE("SAMPLER_NEAREST", "1")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_scale_variable_box)
ADDITIONAL_INFO(compositor_scale_variable)
DEFINE_VALUE("SAMPLER_BOX", "1")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_scale_variable_bspline)
ADDITIONAL_INFO(compositor_scale_variable)
DEFINE_VALUE("SAMPLER_BSPLINE", "1")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
