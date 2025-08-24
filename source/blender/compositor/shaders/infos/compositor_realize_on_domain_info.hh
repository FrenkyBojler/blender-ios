/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_shared)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float4x4, imat)
PUSH_CONSTANT(float2, wh)
SAMPLER(0, sampler2D, input_tx)
COMPUTE_SOURCE("compositor_realize_on_domain.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_fast)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float4x4, imat)
SAMPLER(0, sampler2D, input_tx)
COMPUTE_SOURCE("compositor_realize_on_domain_fast.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_fast_float4)
ADDITIONAL_INFO(compositor_realize_on_domain_fast)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_fast_float2)
ADDITIONAL_INFO(compositor_realize_on_domain_fast)
IMAGE(0, SFLOAT_16_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_fast_float)
ADDITIONAL_INFO(compositor_realize_on_domain_fast)
IMAGE(0, SFLOAT_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_box)
ADDITIONAL_INFO(compositor_realize_on_domain_shared)
DEFINE_VALUE("SAMPLER_BOX", "1")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_box_float4)
ADDITIONAL_INFO(compositor_realize_on_domain_box)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_box_float2)
ADDITIONAL_INFO(compositor_realize_on_domain_box)
IMAGE(0, SFLOAT_16_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_box_float)
ADDITIONAL_INFO(compositor_realize_on_domain_box)
IMAGE(0, SFLOAT_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_bspline)
ADDITIONAL_INFO(compositor_realize_on_domain_shared)
DEFINE_VALUE("SAMPLER_BSPLINE", "1")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_bspline_float4)
ADDITIONAL_INFO(compositor_realize_on_domain_bspline)
IMAGE(0, SFLOAT_16_16_16_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_bspline_float2)
ADDITIONAL_INFO(compositor_realize_on_domain_bspline)
IMAGE(0, SFLOAT_16_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(compositor_realize_on_domain_bspline_float)
ADDITIONAL_INFO(compositor_realize_on_domain_bspline)
IMAGE(0, SFLOAT_16, write, image2D, domain_img)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
