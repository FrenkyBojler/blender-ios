/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "draw_defines.hh"
#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Armatureskin Deform Linear
 * \{ */
GPU_SHADER_CREATE_INFO(draw_armature_skinning_lbs)
LOCAL_GROUP_SIZE(SKINNING_LOCAL_SIZE)

STORAGE_BUF(0, read, uint2, indices_buf[])
STORAGE_BUF(1, read, uint2, weights_buf[])
STORAGE_BUF(2, read, float4x4, bonemat_buf[])
STORAGE_BUF(3, read, float4, pos_buf[])
STORAGE_BUF(4, read, float2, nor_buf[])
STORAGE_BUF(5, read, float4, tan_buf[])
STORAGE_BUF(6, write, float4, out_skinned_pos[])
STORAGE_BUF(7, write, float4, out_skinned_nor[])
STORAGE_BUF(8, write, float4, out_skinned_tan[])

PUSH_CONSTANT(int, vertex_count)

COMPUTE_SOURCE("draw_armature_skinning_lbs.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
/** \} */