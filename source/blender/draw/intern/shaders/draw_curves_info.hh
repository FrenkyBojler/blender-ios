/* SPDX-FileCopyrightText: 2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"

#  include "draw_attribute_shader_shared.hh"
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

GPU_SHADER_CREATE_INFO(draw_curves_data)
LOCAL_GROUP_SIZE(64)
/* Offsets giving the start and end of the curve. */
STORAGE_BUF(0, read, int, points_by_curve_buf[])
STORAGE_BUF(1, read, int, curves_type_buf[])
STORAGE_BUF(2, read, uint, curves_resolution_buf[])
STORAGE_BUF(3, read, int, evaluated_points_by_curve_buf[])
/* Bezier handles (if needed). */
STORAGE_BUF(4, read, float, handles_positions_left_buf[])
STORAGE_BUF(5, read, float, handles_positions_right_buf[])
STORAGE_BUF(6, read, int, bezier_offsets_buf[])
/* Nurbs (alias of other buffers).  */
// STORAGE_BUF(2, read, uint, curves_order_buf[])
// STORAGE_BUF(4, read, float, basis_cache_buf[])
// STORAGE_BUF(5, read, float, control_weights_buf[])
// STORAGE_BUF(6, read, int, basis_cache_offset_buf[])
PUSH_CONSTANT(int, curves_count)
PUSH_CONSTANT(bool, compute_length_and_time)
PUSH_CONSTANT(bool, use_point_weight)
SPECIALIZATION_CONSTANT(int, evaluated_type, 0)
TYPEDEF_SOURCE("draw_attribute_shader_shared.hh")
COMPUTE_SOURCE("draw_curves_interpolation_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(draw_curves_interpolate_position)
ADDITIONAL_INFO(draw_curves_data)
/* Attributes. */
STORAGE_BUF(7, read, float, positions_buf[])
STORAGE_BUF(8, read, float, radii_buf[])
/* Outputs. */
STORAGE_BUF(9, read_write, float4, evaluated_positions_radii_buf[])
STORAGE_BUF(10, read_write, float, evaluated_time_buf[])
STORAGE_BUF(11, write, float, curves_length_buf[])
COMPUTE_FUNCTION("evaluate_position_radius")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(draw_curves_interpolate_float4_attribute)
ADDITIONAL_INFO(draw_curves_data)
STORAGE_BUF(7, read, StoredFloat4, attribute_float4_buf[])
STORAGE_BUF(8, read_write, StoredFloat4, evaluated_float4_buf[])
COMPUTE_FUNCTION("evaluate_attribute_float4")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(draw_curves_interpolate_float3_attribute)
ADDITIONAL_INFO(draw_curves_data)
STORAGE_BUF(7, read, StoredFloat3, attribute_float3_buf[])
STORAGE_BUF(8, read_write, StoredFloat3, evaluated_float3_buf[])
COMPUTE_FUNCTION("evaluate_attribute_float3")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(draw_curves_interpolate_float2_attribute)
ADDITIONAL_INFO(draw_curves_data)
STORAGE_BUF(7, read, StoredFloat2, attribute_float2_buf[])
STORAGE_BUF(8, read_write, StoredFloat2, evaluated_float2_buf[])
COMPUTE_FUNCTION("evaluate_attribute_float2")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(draw_curves_interpolate_float_attribute)
ADDITIONAL_INFO(draw_curves_data)
STORAGE_BUF(7, read, StoredFloat, attribute_float_buf[])
STORAGE_BUF(8, read_write, StoredFloat, evaluated_float_buf[])
COMPUTE_FUNCTION("evaluate_attribute_float")
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
