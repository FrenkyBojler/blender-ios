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
struct MeshBatchCache;
struct MeshBufferCache;

namespace blender::draw {

struct DRWSkinningCache {
  /* input buffer rest position mesh index influences */
  gpu::VertBuf *in_indices_buf;
  /* input buffer rest position mesh weight influences */
  gpu::VertBuf *in_weights_buf;
  /* input buffer of armature bone matrices */
  gpu::VertBuf *in_bonemat_buf;
  /* input buffer rest position mesh position */
  gpu::VertBuf *in_vertpos_buf;
  /* input buffer rest position mesh normals */
  gpu::VertBuf *in_vertnor_buf;
  /* input buffer rest position mesh tangents */
  gpu::VertBuf *in_verttan_buf;

  /* Deformation shader */
  gpu::Shader *skin_shader;

  /* AABB or boundingbox shader for GPU Deformation */
  gpu::Shader *bounds_shader;

  gpu::StorageBuf *bounds_result_buf;
  gpu::StorageBuf *original_bounds_buf;

  /* Bone extraction buffers */
  float *bonedata_mat;

  /* Mesh extraction buffers */
  float *meshdata_pos;
  float *meshdata_nor;
  float *meshdata_tan;

  uint32_t *meshdata_wgt;
  uint32_t *meshdata_idx;

  /* ArmatureMod deform flag getter*/
  short cached_deform_flag;
  /* Mesh corner count getter for workgroup dispatch*/
  int corner_nums;
  /* bone listbase count getter*/
  int bone_count;
  /* Returns true for if all shader buffers were created */
  bool buffers_valid;
  /* Returns true if mesh extraction succeded*/
  bool vertex_data_packed;
};

bool draw_skinning_is_available(const Object *ob);

void draw_skinning_cache_free(DRWSkinningCache &cache);
void draw_free_skinning_runtime_cache(Object &ob);

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

}  // namespace blender::draw
