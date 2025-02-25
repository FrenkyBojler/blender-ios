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

GPU_SHADER_CREATE_INFO(subdiv_custom_data_interp_4d_u16)
DO_STATIC_COMPILATION()
ADDITIONAL_INFO_EXPAND(subdiv_custom_data_base,
                       subdiv_custom_data_4d_base,
                       subdiv_custom_data_u16_base)
GPU_SHADER_CREATE_END()

#define SUBDIV_CUSTOM_DATA_DIMENSION_VARIATIONS(prefix, ...) \
  CREATE_INFO_VARIANT(prefix##_1d, subdiv_custom_data_1d_base, __VA_ARGS__) \
  CREATE_INFO_VARIANT(prefix##_2d, subdiv_custom_data_2d_base, __VA_ARGS__) \
  CREATE_INFO_VARIANT(prefix##_3d, subdiv_custom_data_3d_base, __VA_ARGS__) \
  CREATE_INFO_VARIANT(prefix##_4d, subdiv_custom_data_4d_base, __VA_ARGS__)

#define SUBDIV_CUSTOM_DATA_DATA_TYPE_VARIATIONS(prefix, ...) \
  SUBDIV_CUSTOM_DATA_DIMENSION_VARIATIONS(prefix##_i32, subdiv_custom_data_i32_base, __VA_ARGS__) \
  SUBDIV_CUSTOM_DATA_DIMENSION_VARIATIONS(prefix##_f32, subdiv_custom_data_f32_base, __VA_ARGS__)

SUBDIV_CUSTOM_DATA_DATA_TYPE_VARIATIONS(subdiv_custom_data_interp, subdiv_custom_data_base)

#undef SUBDIV_CUSTOM_DATA_DATA_TYPE_VARIATIONS
#undef SUBDIV_CUSTOM_DATA_DIMENSION_VARIATIONS

/** \} */
