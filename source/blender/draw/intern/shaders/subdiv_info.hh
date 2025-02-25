/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_glsl_cpp_stubs.hh"

#  include "draw_subdiv_shader_shared.hh"
#endif

#include "gpu_shader_create_info.hh"

#include "draw_subdiv_defines.hh"

GPU_SHADER_CREATE_INFO(subdiv_base)
LOCAL_GROUP_SIZE(SUBDIV_GROUP_SIZE)
TYPEDEF_SOURCE("draw_subdiv_shader_shared.hh")
UNIFORM_BUF(SHADER_DATA_BUF_SLOT, DRWSubdivUboStorage, shader_data)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_polygon_offset_base)
DEFINE("SUBDIV_POLYGON_OFFSET")
STORAGE_BUF(SUBDIV_FACE_OFFSET_BUF_SLOT, READ, uint, subdiv_face_offset[])
ADDITIONAL_INFO(subdiv_base)
GPU_SHADER_CREATE_END()

/* -------------------------------------------------------------------- */
/** \name Loop normals
 * \{ */

GPU_SHADER_CREATE_INFO(subdiv_loop_normals)
DO_STATIC_COMPILATION()
STORAGE_BUF(LOOP_NORMALS_POS_NOR_BUF_SLOT, READ, PosNorLoop, pos_nor[])
STORAGE_BUF(LOOP_NORMALS_EXTRA_COARSE_FACE_DATA_BUF_SLOT, READ, uint, extra_coarse_face_data[])
STORAGE_BUF(LOOP_NORMALS_INPUT_VERT_ORIG_INDEX_BUF_SLOT, READ, int, input_vert_origindex[])
STORAGE_BUF(LOOP_NORMALS_OUTPUT_LNOR_BUF_SLOT, WRITE, LoopNormal, output_lnor[])
COMPUTE_SOURCE("subdiv_vbo_lnor_comp.glsl")
ADDITIONAL_INFO(subdiv_polygon_offset_base)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Triangle indices
 * \{ */

GPU_SHADER_CREATE_INFO(subdiv_tris_single_material)
DO_STATIC_COMPILATION()
DEFINE("SINGLE_MATERIAL")
STORAGE_BUF(TRIS_EXTRA_COARSE_FACE_DATA_BUF_SLOT, READ, uint, extra_coarse_face_data[])
STORAGE_BUF(TRIS_OUTPUT_TRIS_BUF_SLOT, WRITE, uint, output_tris[])
COMPUTE_SOURCE("subdiv_ibo_tris_comp.glsl")
ADDITIONAL_INFO(subdiv_polygon_offset_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_tris_multiple_materials)
DO_STATIC_COMPILATION()
STORAGE_BUF(TRIS_EXTRA_COARSE_FACE_DATA_BUF_SLOT, READ, uint, extra_coarse_face_data[])
STORAGE_BUF(TRIS_OUTPUT_TRIS_BUF_SLOT, WRITE, uint, output_tris[])
STORAGE_BUF(TRIS_FACE_MAT_OFFSET, READ, uint, face_mat_offset[])
COMPUTE_SOURCE("subdiv_ibo_tris_comp.glsl")
ADDITIONAL_INFO(subdiv_polygon_offset_base)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Line indices
 * \{ */

GPU_SHADER_CREATE_INFO(subdiv_lines)
DO_STATIC_COMPILATION()
STORAGE_BUF(LINES_INPUT_EDGE_DRAW_FLAG_BUF_SLOT, READ, int, input_edge_draw_flag[])
STORAGE_BUF(LINES_EXTRA_COARSE_FACE_DATA_BUF_SLOT, READ, uint, extra_coarse_face_data[])
STORAGE_BUF(LINES_OUTPUT_LINES_BUF_SLOT, WRITE, uint, output_lines[])
COMPUTE_SOURCE("subdiv_ibo_lines_comp.glsl")
ADDITIONAL_INFO(subdiv_polygon_offset_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_lines_loose)
DO_STATIC_COMPILATION()
DEFINE("LINES_LOOSE")
STORAGE_BUF(LINES_OUTPUT_LINES_BUF_SLOT, WRITE, uint, output_lines[])
STORAGE_BUF(LINES_LINES_LOOSE_FLAGS, READ, uint, lines_loose_flags[])
COMPUTE_SOURCE("subdiv_ibo_lines_comp.glsl")
ADDITIONAL_INFO(subdiv_base)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edge data for object mode wireframe
 * \{ */

GPU_SHADER_CREATE_INFO(subdiv_edge_fac_base)
ADDITIONAL_INFO(subdiv_base)
STORAGE_BUF(EDGE_FAC_POS_NOR_BUF_SLOT, READ, PosNorLoop, pos_nor[])
STORAGE_BUF(EDGE_FAC_EDGE_DRAW_FLAG_BUF_SLOT, READ, uint, input_edge_draw_flag[])
STORAGE_BUF(EDGE_FAC_POLY_OTHER_MAP_BUF_SLOT, READ, int, input_poly_other_map[])
COMPUTE_SOURCE("subdiv_vbo_edge_fac_comp.glsl")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_edge_fac)
DO_STATIC_COMPILATION()
STORAGE_BUF(EDGE_FAC_EDGE_FAC_BUF_SLOT, WRITE, uint, output_edge_fac[])
ADDITIONAL_INFO(subdiv_edge_fac_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_edge_fac_amd_legacy)
DO_STATIC_COMPILATION()
DEFINE("GPU_AMD_DRIVER_BYTE_BUG")
STORAGE_BUF(EDGE_FAC_EDGE_FAC_BUF_SLOT, WRITE, float, output_edge_fac[])
ADDITIONAL_INFO(subdiv_edge_fac_base)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Custom data
 * \{ */

GPU_SHADER_CREATE_INFO(subdiv_custom_data_base)
COMPUTE_SOURCE("subdiv_custom_data_interp_comp.glsl")
STORAGE_BUF(CUSTOM_DATA_FACE_PTEX_OFFSET_BUF_SLOT, READ, uint, face_ptex_offset[])
STORAGE_BUF(CUSTOM_DATA_PATCH_COORDS_BUF_SLOT, READ, BlenderPatchCoord, patch_coords[])
STORAGE_BUF(CUSTOM_DATA_EXTRA_COARSE_FACE_DATA_BUF_SLOT, READ, uint, extra_coarse_face_data[])
ADDITIONAL_INFO(subdiv_polygon_offset_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_u16_base)
DEFINE("GPU_COMP_U16")
STORAGE_BUF(CUSTOM_DATA_SOURCE_DATA_BUF_SLOT, READ, uint, src_data[])
STORAGE_BUF(CUSTOM_DATA_DESTINATION_DATA_BUF_SLOT, WRITE, uint, dst_data[])
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_i32_base)
DEFINE("GPU_COMP_I32")
STORAGE_BUF(CUSTOM_DATA_SOURCE_DATA_BUF_SLOT, READ, int, src_data[])
STORAGE_BUF(CUSTOM_DATA_DESTINATION_DATA_BUF_SLOT, WRITE, int, dst_data[])
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_f32_base)
DEFINE("GPU_COMP_F32")
STORAGE_BUF(CUSTOM_DATA_SOURCE_DATA_BUF_SLOT, READ, float, src_data[])
STORAGE_BUF(CUSTOM_DATA_DESTINATION_DATA_BUF_SLOT, WRITE, float, dst_data[])
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_1d_base)
DEFINE("DIMENSIONS_1")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_2d_base)
DEFINE("DIMENSIONS_2")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_3d_base)
DEFINE("DIMENSIONS_3")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_4d_base)
DEFINE("DIMENSIONS_4")
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_interp_1d_i32)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(subdiv_custom_data_base)
ADDITIONAL_INFO(subdiv_custom_data_1d_base)
ADDITIONAL_INFO(subdiv_custom_data_i32_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_interp_1d_f32)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(subdiv_custom_data_base)
ADDITIONAL_INFO(subdiv_custom_data_1d_base)
ADDITIONAL_INFO(subdiv_custom_data_f32_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_interp_2d_i32)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(subdiv_custom_data_base)
ADDITIONAL_INFO(subdiv_custom_data_2d_base)
ADDITIONAL_INFO(subdiv_custom_data_i32_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_interp_2d_f32)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(subdiv_custom_data_base)
ADDITIONAL_INFO(subdiv_custom_data_2d_base)
ADDITIONAL_INFO(subdiv_custom_data_f32_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_interp_3d_i32)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(subdiv_custom_data_base)
ADDITIONAL_INFO(subdiv_custom_data_3d_base)
ADDITIONAL_INFO(subdiv_custom_data_i32_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_interp_3d_f32)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(subdiv_custom_data_base)
ADDITIONAL_INFO(subdiv_custom_data_3d_base)
ADDITIONAL_INFO(subdiv_custom_data_f32_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_interp_4d_u16)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(subdiv_custom_data_base)
ADDITIONAL_INFO(subdiv_custom_data_4d_base)
ADDITIONAL_INFO(subdiv_custom_data_u16_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_interp_4d_i32)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(subdiv_custom_data_base)
ADDITIONAL_INFO(subdiv_custom_data_4d_base)
ADDITIONAL_INFO(subdiv_custom_data_i32_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_custom_data_interp_4d_f32)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO(subdiv_custom_data_base)
ADDITIONAL_INFO(subdiv_custom_data_4d_base)
ADDITIONAL_INFO(subdiv_custom_data_f32_base)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt data
 * \{ */

GPU_SHADER_CREATE_INFO(subdiv_sculpt_data)
DO_STATIC_COMPILATION()
STORAGE_BUF(SCULPT_DATA_SCULPT_MASK_BUF_SLOT, READ, float, sculpt_mask[])
STORAGE_BUF(SCULPT_DATA_SCULPT_FACE_SET_COLOR_BUF_SLOT, READ, uint, sculpt_face_set_color[])
STORAGE_BUF(SCULPT_DATA_SCULPT_DATA_BUF_SLOT, WRITE, SculptData, sculpt_data[])
COMPUTE_SOURCE("subdiv_vbo_sculpt_data_comp.glsl")
ADDITIONAL_INFO(subdiv_base)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name UV Stretch overlays
 * \{ */

GPU_SHADER_CREATE_INFO(subdiv_edituv_stretch_angle)
DO_STATIC_COMPILATION()
STORAGE_BUF(STRETCH_ANGLE_POS_NOR_BUF_SLOT, READ, PosNorLoop, pos_nor[])
STORAGE_BUF(STRETCH_ANGLE_UVS_BUF_SLOT, READ, packed_float2, uvs[])
STORAGE_BUF(STRETCH_ANGLE_UV_STRETCHES_BUF_SLOT, WRITE, UVStretchAngle, uv_stretches[])
COMPUTE_SOURCE("subdiv_vbo_edituv_strech_angle_comp.glsl")
ADDITIONAL_INFO(subdiv_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_edituv_stretch_area)
DO_STATIC_COMPILATION()
STORAGE_BUF(STRETCH_AREA_COARSE_STRETCH_AREA_BUF_SLOT, READ, float, coarse_stretch_area[])
STORAGE_BUF(STRETCH_AREA_SUBDIV_STRETCH_AREA_BUF_SLOT, WRITE, float, subdiv_stretch_area[])
COMPUTE_SOURCE("subdiv_vbo_edituv_strech_area_comp.glsl")
ADDITIONAL_INFO(subdiv_polygon_offset_base)
GPU_SHADER_CREATE_END()

/** \} */

/* -------------------------------------------------------------------- */
/** \name Normals
 * \{ */

GPU_SHADER_CREATE_INFO(subdiv_normals_accumulate)
DO_STATIC_COMPILATION()
STORAGE_BUF(NORMALS_ACCUMULATE_POS_NOR_BUF_SLOT, READ, PosNorLoop, pos_nor[])
STORAGE_BUF(NORMALS_ACCUMULATE_FACE_ADJACENCY_OFFSETS_BUF_SLOT,
            READ,
            uint,
            face_adjacency_offsets[])
STORAGE_BUF(NORMALS_ACCUMULATE_FACE_ADJACENCY_LISTS_BUF_SLOT, READ, uint, face_adjacency_lists[])
STORAGE_BUF(NORMALS_ACCUMULATE_VERTEX_LOOP_MAP_BUF_SLOT, READ, uint, vert_loop_map[])
STORAGE_BUF(NORMALS_ACCUMULATE_NORMALS_BUF_SLOT, WRITE, packed_float3, normals[])
COMPUTE_SOURCE("subdiv_normals_accumulate_comp.glsl")
ADDITIONAL_INFO(subdiv_base)
GPU_SHADER_CREATE_END()

GPU_SHADER_CREATE_INFO(subdiv_normals_finalize)
DO_STATIC_COMPILATION()
STORAGE_BUF(NORMALS_FINALIZE_VERTEX_NORMALS_BUF_SLOT, READ, packed_float3, vertex_normals[])
STORAGE_BUF(NORMALS_FINALIZE_VERTEX_LOOP_MAP_BUF_SLOT, READ, uint, vert_loop_map[])
STORAGE_BUF(NORMALS_FINALIZE_POS_NOR_BUF_SLOT, READ_WRITE, PosNorLoop, pos_nor[])
COMPUTE_SOURCE("subdiv_normals_finalize_comp.glsl")
ADDITIONAL_INFO(subdiv_base)
GPU_SHADER_CREATE_END()

/* Meshes can have (custom) split normals as loop attribute. */
GPU_SHADER_CREATE_INFO(subdiv_custom_normals_finalize)
DO_STATIC_COMPILATION()
DEFINE("CUSTOM_NORMALS")
STORAGE_BUF(NORMALS_FINALIZE_CUSTOM_NORMALS_BUF_SLOT, READ, CustomNormal, custom_normals[])
STORAGE_BUF(NORMALS_FINALIZE_POS_NOR_BUF_SLOT, READ_WRITE, PosNorLoop, pos_nor[])
COMPUTE_SOURCE("subdiv_normals_finalize_comp.glsl")
ADDITIONAL_INFO(subdiv_base)
GPU_SHADER_CREATE_END()

/** \} */
