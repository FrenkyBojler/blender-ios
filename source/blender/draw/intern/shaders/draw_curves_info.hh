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

GPU_SHADER_CREATE_INFO(draw_curves_topology_compute)
LOCAL_GROUP_SIZE(64)
/* Offsets giving the start and end of the curve. */
STORAGE_BUF(0, read, uint, curves_offsets_buf[])
STORAGE_BUF(1, read, uint, curves_type_buf[])
STORAGE_BUF(2, read, uint, curves_resolution_buf[])
STORAGE_BUF(3, read, uint, curves_cyclic_buf[])
STORAGE_BUF(4, write, uint, curves_length_buf[])
STORAGE_BUF(5, write, float4, points_weights_buf[])
STORAGE_BUF(6, write, uint, points_curve_id_buf[])
STORAGE_BUF(7, write, uint, points_time_buf[])
/* Buffer cleared to 0 before this dispatch. */
STORAGE_BUF(6, read_write, uint, atomic_point_counter[])
PUSH_CONSTANT(uint, curves_count)
PUSH_CONSTANT(bool, is_ribbon)
PUSH_CONSTANT(bool, compute_length)
COMPUTE_SOURCE("draw_curves_topology_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
