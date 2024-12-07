/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"

#  include "draw_common_shader_shared.hh"
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(draw_globals)
TYPEDEF_SOURCE("draw_common_shader_shared.hh")
UNIFORM_BUF_FREQ(OVERLAY_GLOBALS_SLOT, GlobalsUboStorage, globalsBlock, PASS)
GPU_SHADER_CREATE_END()
