/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief GPU Acceleration header
 */
#pragma once

#include "draw_cache_extract.hh"
#include "draw_shader_shared.hh"

#include "GPU_context.hh"
#include "GPU_storage_buffer.hh"

#include "gpu_capabilities_private.hh"

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

  gpu::Shader *compute_shader;

  gpu::VertBuf *in_indices_buf;
  gpu::VertBuf *in_weights_buf;
  gpu::VertBuf *in_bonemat_buf;
  gpu::VertBuf *in_vertpos_buf;
  gpu::VertBuf *in_vertnor_buf;
  gpu::VertBuf *in_verttan_buf;

  gpu::StorageBuf *in_bonedq_buf;

  float *meshdata_pos;
  float *meshdata_nor;
  float *meshdata_tan;

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

void draw_skinning_extract_pos_nor_tan(gpu::VertBuf *vbo_pos,
                                       gpu::VertBuf *vbo_nor,
                                       gpu::VertBuf *vbo_tan,
                                       const DRWSkinningCache &cache);

void draw_skinning_compute_bounds(Mesh *mesh,
                                  const DRWSkinningCache &cache,
                                  gpu::VertBuf *skinned_positions_vbo);

void draw_skinning_bounds_cleanup();

}  // namespace blender::draw
