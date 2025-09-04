/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(vk_backbuffer_blit)
LOCAL_GROUP_SIZE(16, 16)
PUSH_CONSTANT(float, sdr_scale)
COMPUTE_SOURCE("vk_backbuffer_blit_comp.glsl")
ADDITIONAL_INFO(gpu_srgb_to_framebuffer_space)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(vk_backbuffer_blit_extended_linear)
IMAGE(0, SFLOAT_16_16_16_16, read, Float2D, src_img)
IMAGE(1, SFLOAT_16_16_16_16, write, Float2D, dst_img)
ADDITIONAL_INFO(vk_backbuffer_blit)
DEFINE("COLOR_SPACE_EXTENDED_SRGB_LINEAR")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(vk_backbuffer_blit_extended_linear_gamma22)
ADDITIONAL_INFO(vk_backbuffer_blit_extended_linear)
DEFINE("USE_GAMMA22")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* For VK_COLOR_SPACE_HDR10_ST2084_EXT (HDR10 (BT2020) using SMPTE ST2084 PQ).  */
GPU_SHADER_CREATE_INFO(vk_backbuffer_blit_hdr10_st2084)
ADDITIONAL_INFO(vk_backbuffer_blit)
DEFINE("COLOR_SPACE_HDR10_ST2084")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(vk_backbuffer_blit_hdr10_st2084_sfloat16)
IMAGE(0, SFLOAT_16_16_16_16, read, Float2D, src_img)
IMAGE(1, SFLOAT_16_16_16_16, write, Float2D, dst_img)
ADDITIONAL_INFO(vk_backbuffer_blit_hdr10_st2084)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(vk_backbuffer_blit_hdr10_st2084_b10g10r10)
IMAGE(0, UNORM_10_10_10_2, read, Float2D, src_img)
IMAGE(1, UNORM_10_10_10_2, write, Float2D, dst_img)
ADDITIONAL_INFO(vk_backbuffer_blit_hdr10_st2084)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(vk_backbuffer_blit_hdr10_st2084_r10g10b10)
IMAGE(0, UNORM_R10_G10_B10_2, read, Float2D, src_img)
IMAGE(1, UNORM_R10_G10_B10_2, write, Float2D, dst_img)
ADDITIONAL_INFO(vk_backbuffer_blit_hdr10_st2084)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
