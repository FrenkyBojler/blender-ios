/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_masked_maximum)
LOCAL_GROUP_SIZE(16, 16)
SAMPLER(0, sampler2D, input_image_tx)
SAMPLER(1, sampler2D, input_x_scale_tx)
SAMPLER(2, sampler2D, input_y_scale_ty)
SAMPLER(3, sampler2D, input_falloff_tx)
IMAGE(0, GPU_R16F, write, image2D, output_img)
COMPUTE_SOURCE("compositor_masked_maximum.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
