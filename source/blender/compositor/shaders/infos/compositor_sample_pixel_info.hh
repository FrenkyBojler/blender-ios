/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_sample_pixel)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(int2, texel)
SAMPLER(0, sampler2D, input_tx)
IMAGE(0, GPU_RGBA32F, write, image2D, output_img)
COMPUTE_SOURCE("compositor_sample_pixel.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
