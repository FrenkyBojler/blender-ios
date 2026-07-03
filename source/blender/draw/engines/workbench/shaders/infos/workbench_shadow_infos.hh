/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once

#  include "BLI_utildefines_variadic.hh"

#  include "gpu_shader_compat.hh"

#  include "draw_object_infos_infos.hh"
#  include "draw_view_infos.hh"
#  include "gpu_index_load_infos.hh"
#  include "gpu_shader_fullscreen_infos.hh"

#  include "workbench_shader_shared.hh"
#endif

#ifdef GLSL_CPP_STUBS
#  define DYNAMIC_PASS_SELECTION
#endif

#include "draw_defines.hh"

#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Raytracing
 * \{ */

GPU_SHADER_CREATE_INFO(workbench_shadow_raytrace)
UNIFORM_BUF(1, ShadowPassData, pass_data)
SAMPLER(2, sampler2DDepth, depth_tx)
SAMPLER(3, sampler2D, normal_tx)
ACCELERATION_STRUCTURE(0, shadow_as)
TYPEDEF_SOURCE("workbench_shader_shared.hh")
FRAGMENT_SOURCE("workbench_shadow_raytrace_frag.glsl")
ADDITIONAL_INFO(gpu_fullscreen)
ADDITIONAL_INFO(draw_view)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/** \} */
