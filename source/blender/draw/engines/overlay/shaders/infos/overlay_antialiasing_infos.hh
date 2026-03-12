/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "overlay_fullscreen_infos.hh"
#  include "overlay_shader_shared.hh"
#endif

#include "overlay_common_infos.hh"

GPU_SHADER_CREATE_INFO(overlay_xray_fade)
DO_STATIC_COMPILATION()
SAMPLER(0, sampler2DDepth, depth_tx)
SAMPLER(1, sampler2DDepth, xray_depth_tx)
PUSH_CONSTANT(float, opacity)
FRAGMENT_OUT(0, float4, frag_color)
FRAGMENT_SOURCE("overlay_xray_fade_frag.glsl")
ADDITIONAL_INFO(overlay_fullscreen)
SAMPLER(2, sampler2DDepth, xray_depth_txInfront)
SAMPLER(3, sampler2DDepth, depth_txInfront)
GPU_SHADER_CREATE_END()
