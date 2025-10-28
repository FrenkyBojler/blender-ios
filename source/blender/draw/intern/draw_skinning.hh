/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief GPU Acceleration header
 */
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include <tbb/parallel_for.h>

#include "BLT_translation.hh"

#include "DNA_armature_types.h"
#include "DNA_defaults.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_customdata.hh"
#include "BKE_deform.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_types.hh"
#include "BKE_modifier.hh"
#include "DEG_depsgraph_query.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "WM_api.hh"

#include "MEM_guardedalloc.h"

#include "draw_cache_extract.hh"

#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"

#include "bmesh.hh"
#include "draw_cache_extract.hh"
#include "draw_shader_shared.hh"
#include "gpu_capabilities_private.hh"
#include "gpu_shader_create_info.hh"
#include "mesh_extractors/extract_mesh.hh"


using namespace blender::gpu::shader;
using namespace blender::gpu;

namespace blender::gpu {
class IndexBuf;
class UniformBuf;
class VertBuf;
}  // namespace blender::gpu

struct GPUVertFormat;

namespace blender::draw {

struct MeshBatchCache;
struct MeshBufferCache;

struct DRWSkinningCache {

  blender::gpu::Shader *compute_shader;

  VertBuf *in_indices_buf;
  VertBuf *in_weights_buf;
  VertBuf *in_bonemat_buf;
  VertBuf *in_vertpos_buf;
  VertBuf *in_vertnor_buf;

  blender::gpu::StorageBuf *in_bonedq_buf;

  VertBuf *out_skinned_pos;
  VertBuf *out_skinned_nor;

  float *meshdata_pos;
  float *meshdata_nor;

  float *bonedata_mat;
  GPUDualQuat *bonedata_dq;

  uint32_t *meshdata_idx;
  uint32_t *meshdata_wgt;

  int corner_nums;
  int bone_count;
  bool buffers_valid;

  bool vertex_data_packed;
  short cached_deform_flag;
};

bool draw_skinning_is_available(const Object *ob);

void draw_skinning_cache_free(DRWSkinningCache &cache);

void DRW_create_skinning(Object &evaluated_object,
                         Mesh &mesh,
                         MeshBatchCache &cache,
                         MeshBufferCache &mbc,
                         const Span<IBOType> ibo_requests,
                         const Span<VBOType> vbo_requests,
                         const bool is_editmode,
                         const bool is_paint_mode,
                         const bool do_final,
                         const bool do_uvedit,
                         const bool do_cage,
                         const ToolSettings *ts,
                         const bool use_hide);

void draw_skinning_extract_pos_nor(VertBuf *vbo_pos,
                                   VertBuf *vbo_nor,
                                   const DRWSkinningCache &cache);

void draw_skinning_compute_bounds(Mesh *mesh,
                                  const DRWSkinningCache &cache,
                                  VertBuf *skinned_positions_vbo);

void draw_skinning_bounds_cleanup();

}  // namespace blender::draw
