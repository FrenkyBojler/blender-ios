/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once

#  include "BLI_utildefines_variadic.h"

#  include "gpu_shader_compat.hh"

#  include "draw_object_infos_infos.hh"
#  include "draw_view_infos.hh"
#  include "gpu_index_load_infos.hh"

#  include "workbench_shader_shared.hh"
#endif

#ifdef GLSL_CPP_STUBS
#  define DYNAMIC_PASS_SELECTION
#endif

#include "draw_defines.hh"

#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Common
 * \{ */

GPU_SHADER_CREATE_INFO(workbench_shadow_visibility_compute_common)
LOCAL_GROUP_SIZE(DRW_VISIBILITY_GROUP_SIZE)
DEFINE_VALUE("DRW_VIEW_LEN", "64")
STORAGE_BUF(0, read, ObjectBounds, bounds_buf[])
UNIFORM_BUF(2, ExtrudedFrustum, extruded_frustum)
PUSH_CONSTANT(int, resource_len)
PUSH_CONSTANT(int, view_len)
PUSH_CONSTANT(int, visibility_word_per_draw)
PUSH_CONSTANT(bool, force_fail_method)
PUSH_CONSTANT(float3, shadow_direction)
TYPEDEF_SOURCE("workbench_shader_shared.hh")
COMPUTE_SOURCE("workbench_shadow_visibility_comp.glsl")
ADDITIONAL_INFO(draw_view)
ADDITIONAL_INFO(draw_view_culling)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(workbench_shadow_visibility_compute_dynamic_pass_type)
ADDITIONAL_INFO(workbench_shadow_visibility_compute_common)
DEFINE("DYNAMIC_PASS_SELECTION")
STORAGE_BUF(1, read_write, uint, pass_visibility_buf[])
STORAGE_BUF(2, read_write, uint, fail_visibility_buf[])
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(workbench_shadow_visibility_compute_static_pass_type)
ADDITIONAL_INFO(workbench_shadow_visibility_compute_common)
STORAGE_BUF(1, read_write, uint, visibility_buf[])
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/** \} */
