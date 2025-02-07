/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_subdiv_defines.hh"

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"
#endif

#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name
 * \{ */

GPU_SHADER_CREATE_INFO(subdiv_base)
LOCAL_GROUP_SIZE(SUBDIV_GROUP_SIZE)
UNIFORM_BUF(0, DRWSubdivUboStorage, shader_data)
TYPEDEF_SOURCE("draw_subdiv_defines.hh")
TYPEDEF_SOURCE("draw_subdiv_shader_shared.hh")
TYPEDEF_SOURCE("subdiv_lib.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_normals_finalize)
DO_STATIC_COMPILATION()
COMPUTE_SOURCE("subdiv_normals_finalize_comp.glsl")
ADDITIONAL_INFO(subdiv_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_normals_finalize)
DO_STATIC_COMPILATION()
DEFINE("CUSTOM_NORMALS")
COMPUTE_SOURCE("subdiv_normals_finalize_comp.glsl")
ADDITIONAL_INFO(subdiv_base)
GPU_SHADER_CREATE_END()
