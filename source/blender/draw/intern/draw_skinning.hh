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

  gpu::UniformBuf *in_armspace_buf;
  gpu::StorageBuf *in_bonedata_buf;

  /* Uniform buffers for target mesh and armature spaces */
  // gpu::UniformBuf *in_armspace_buf;
  // gpu::UniformBuf *in_targspace_buf;
  /* input buffer rest position mesh index influences */
  gpu::VertBuf *in_indices_buf;
  /* input buffer rest position mesh weight influences */
  gpu::VertBuf *in_weights_buf;
  /* input buffer of armature bone matrices */
  gpu::StorageBuf *in_bonemat_buf;
  /* input buffer rest position mesh position */
  gpu::VertBuf *in_vertpos_buf;

  /* Deformation shader */
  gpu::Shader *skin_shader;

  /* AABB/boundingbox shader for GPU Deformation */
  gpu::Shader *bounds_shader;

  gpu::StorageBuf *bounds_result_buf;
  gpu::StorageBuf *original_bounds_buf;

  /* Normal reconstruction buffers */
  gpu::VertBuf *face_adjacency_offsets_buf;
  gpu::VertBuf *face_adjacency_lists_buf;
  gpu::VertBuf *corner_verts_buf;
  gpu::VertBuf *face_offsets_buf;
  gpu::VertBuf *sharp_faces_buf;
  gpu::VertBuf *vert_normals_buf;

  /* Normal reconstruction shaders */
  gpu::Shader *normals_accumulate_shader;
  gpu::Shader *normals_finalize_shader;

  /* Bone extraction data */
  float *bonedata_mat;
  ArmatureSpace *armature_buf;
  BoneData *bonedata_buf;

  /* segments per bone */
  // gpu::StorageBuf *in_bonesegments_buf;
  // /* offset into bonemat_buf per bone */
  // gpu::StorageBuf *in_boneoffsets_buf;
  // /* bone lengths for segment calculation */
  // gpu::StorageBuf *in_bonelengths_buf;
  // /* inverse arm matrices for bone space transform */
  // gpu::StorageBuf *in_bone_invarmmat_buf;

  /* Mesh extraction buffers */
  float *meshdata_pos;
  float *meshdata_nor;
  float *meshdata_tan;

  float *meshdata_wgt;
  uint32_t *meshdata_idx;

  /* ArmatureMod deform flag getter*/
  short cached_deform_flag;
  /* Max influences for skinning shader*/
  short influence_nums;
  /* Mesh corner count getter for workgroup dispatch*/
  int corner_nums;
  /* Mesh vertex count for normal accumulation dispatch */
  int verts_num;
  /* Mesh face count for normal finalize dispatch */
  int faces_num;
  /*This is mainly just for testing against whether skinning is dirty*/
  int edges_num;
  /* bone listbase count getter*/
  int bone_count;
  /* total bendy bone segment count on all Bones */
  int total_segments;
  /* Returns true if mesh extraction succeded*/
  bool vertex_data_packed;
};

// bool draw_is_skinning_available(Object* ob);

void draw_skinning_cache_free(DRWSkinningCache &cache);
void draw_free_skinning_runtime_cache(const Object &ob);

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

void draw_skinning_compute_position(gpu::VertBuf *vbo_pos,
                                    // gpu::VertBuf *vbo_nor,
                                    // gpu::VertBuf *vbo_tan,
                                    const DRWSkinningCache &cache);

void draw_skinning_accumulate_normals(gpu::VertBuf *vbo_pos,
                                      gpu::VertBuf *vbo_nor,
                                      const DRWSkinningCache &cache);

void draw_skinning_finalize_normals(gpu::VertBuf *vbo_pos,
                                    gpu::VertBuf *vbo_nor,
                                    const DRWSkinningCache &cache);

void draw_skinning_compute_bounds(Mesh *mesh,
                                  DRWSkinningCache &cache,
                                  gpu::VertBuf *skinned_positions_vbo);

}  // namespace blender::draw
