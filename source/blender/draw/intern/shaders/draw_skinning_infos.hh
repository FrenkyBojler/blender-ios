/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_SHADER
#  pragma once
#  include "gpu_shader_compat.hh"
#endif

#include "draw_skinning_defines.hh"
#include "gpu_shader_create_info.hh"

/* -------------------------------------------------------------------- */
/** \name Armature Skinning Linear Deform (Computes Vertex Positions Only)
 * \{ */
GPU_SHADER_CREATE_INFO(draw_skinning_linear)
LOCAL_GROUP_SIZE(SKINNING_LOCAL_SIZE)
TYPEDEF_SOURCE("draw_shader_shared.hh")

STORAGE_BUF(0, read, uint, indices_buf[])
STORAGE_BUF(1, read, float, weights_buf[])

STORAGE_BUF(2, read, float4x4, bonemat_buf[])
UNIFORM_BUF(3, float4x4, armspace_buf)
UNIFORM_BUF(4, float4x4, targspace_buf)

STORAGE_BUF(5, read, int, bone_segments[])
STORAGE_BUF(6, read, int, bone_offsets[])
STORAGE_BUF(7, read, float, bone_lengths[])
STORAGE_BUF(8, read, float4x4, bone_invarmmat[])

STORAGE_BUF(9, read, float4, pos_buf[])
// STORAGE_BUF(10, read, float2, nor_buf[])
// STORAGE_BUF(11, read, float4, tan_buf[])

STORAGE_BUF(10, write, float4, out_skinned_pos[])
// STORAGE_BUF(13, write, float4, out_skinned_nor[])
// STORAGE_BUF(14, write, float4, out_skinned_tan[])


PUSH_CONSTANT(int, vertex_count)
PUSH_CONSTANT(int, influence_count)

COMPUTE_SOURCE("draw_skinning_linear.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
/** \} */

/* -------------------------------------------------------------------- */
/** \name Armature Skinning Normals Accumulate
 *
 * Accumulates vertex normals from adjacent faces using Newell's method.
 * This reconstructs normals from deformed positions for 1:1 CPU accuracy.
 * \{ */
GPU_SHADER_CREATE_INFO(draw_skinning_normals_accumulate)
LOCAL_GROUP_SIZE(SKINNING_LOCAL_SIZE)
TYPEDEF_SOURCE("draw_shader_shared.hh")

/* Input: deformed positions (per-corner, from LBS pass) */
STORAGE_BUF(0, read, float4, skinned_pos_buf[])
/* Input: face adjacency data for each vertex */
STORAGE_BUF(1, read, uint, face_adjacency_offsets[])
STORAGE_BUF(2, read, uint, face_adjacency_lists[])
/* Input: corner to vertex mapping (to find vertex in face) */
STORAGE_BUF(3, read, uint, corner_verts_buf[])

/* Output: accumulated vertex normals */
STORAGE_BUF(4, write, float4, vert_normals_buf[])

PUSH_CONSTANT(int, vertex_count)

COMPUTE_SOURCE("draw_skinning_normals_accumulate.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
/** \} */

/* -------------------------------------------------------------------- */
/** \name Armature Skinning Normals Finalize
 *
 * Converts accumulated vertex normals to per-corner loop normals,
 * handling smooth vs flat shading per face.
 * \{ */
GPU_SHADER_CREATE_INFO(draw_skinning_normals_finalize)
LOCAL_GROUP_SIZE(SKINNING_LOCAL_SIZE)
TYPEDEF_SOURCE("draw_shader_shared.hh")

/* Input: deformed positions (per-corner) */
STORAGE_BUF(0, read, float4, skinned_pos_buf[])
/* Input: accumulated vertex normals */
STORAGE_BUF(1, read, float4, vert_normals_buf[])
/* Input: corner to vertex mapping */
STORAGE_BUF(2, read, uint, corner_verts_buf[])
/* Input: face offsets (start corner index for each face) */
STORAGE_BUF(3, read, uint, face_offsets_buf[])
/* Input: face smooth flags (bit per face) */
STORAGE_BUF(4, read, uint, sharp_faces_buf[])

/* Output: final per-corner normals */
STORAGE_BUF(5, write, float4, out_skinned_nor[])

PUSH_CONSTANT(int, face_count)

COMPUTE_SOURCE("draw_skinning_normals_finalize.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
/** \} */

/* -------------------------------------------------------------------- */
/** \name Armatureskin Deform AABB
 * \{ */
GPU_SHADER_CREATE_INFO(draw_skinning_aabb)
LOCAL_GROUP_SIZE(SKINNING_LOCAL_SIZE)

STORAGE_BUF(0, read, float4, skinned_positions[])
STORAGE_BUF(1, read, float4, original_bounds_buf[])
STORAGE_BUF(2, write, uint, bounds_result_buf[])

PUSH_CONSTANT(int, vertex_count_aabb)

COMPUTE_SOURCE("draw_skinning_aabb.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
/** \} */