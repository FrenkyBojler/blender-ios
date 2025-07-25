/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"

#  include "draw_object_infos_info.hh"

#  define DRW_HAIR_INFO
#endif

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(draw_curves_topology)
LOCAL_GROUP_SIZE(64)
/* Offsets giving the start and end of the curve. */
STORAGE_BUF(0, read, uint, evaluated_offsets_buf[])
STORAGE_BUF(1, write, uint, indirection_buf[])
PUSH_CONSTANT(int, curves_count)
PUSH_CONSTANT(bool, is_ribbon_topology)
COMPUTE_SOURCE("draw_curves_topology_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(draw_curves_interpolation)
LOCAL_GROUP_SIZE(64)
/* Offsets giving the start and end of the curve. */
STORAGE_BUF(0, read, uint, curves_offsets_buf[])
STORAGE_BUF(1, read, uint, curves_type_buf[])
STORAGE_BUF(2, read, uint, curves_resolution_buf[])
STORAGE_BUF(3, read, uint, curves_evaluated_offsets_buf[])
STORAGE_BUF(4, read, float, points_pos_buf[])
STORAGE_BUF(5, read, float, points_rad_buf[])
STORAGE_BUF(6, write, float4, points_pos_rad_buf[])
STORAGE_BUF(7, read_write, float, points_time_buf[])
STORAGE_BUF(8, write, float, curves_length_buf[])
PUSH_CONSTANT(int, curves_count)
COMPUTE_SOURCE("draw_curves_interpolation_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(draw_curves_test)
STORAGE_BUF(0, write, float, result_pos_buf[])
STORAGE_BUF(1, write, int4, result_indices_buf[])
VERTEX_SOURCE("draw_curves_test.glsl")
FRAGMENT_SOURCE("draw_curves_test.glsl")
ADDITIONAL_INFO(draw_curves_infos)
ADDITIONAL_INFO(draw_curves)
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
