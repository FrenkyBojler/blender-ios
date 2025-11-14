/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"

#  include "eevee_uniform_infos.hh"
#  include "gpu_shader_fullscreen_infos.hh"
#endif

#include "eevee_defines.hh"
#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(eevee_transparency_resolve)
DO_STATIC_COMPILATION()
FRAGMENT_OUT(0, float4, out_radiance)
FRAGMENT_OUT(1, float4, out_transmittance)
SAMPLER(0, sampler2D, transparency_r_tx)
SAMPLER(1, sampler2D, transparency_g_tx)
SAMPLER(2, sampler2D, transparency_b_tx)
SAMPLER(3, sampler2D, transparency_a_tx)
FRAGMENT_SOURCE("eevee_transparency_resolve_frag.glsl")
ADDITIONAL_INFO(gpu_fullscreen)
ADDITIONAL_INFO(eevee_global_ubo)
GPU_SHADER_CREATE_END()
