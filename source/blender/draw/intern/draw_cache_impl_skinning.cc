/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief GPU Acceleration for Armature modifier and Shape keys
 */
#include "BKE_action.hh"
#include "BKE_armature.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_tangent.hh"
#include "BKE_modifier.hh"

#include "BLI_array_utils.hh"
#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_task.hh"

#include "DNA_armature_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_userdef_types.h"
#include "DNA_vec_types.h"

#include "draw_cache_extract.hh"
#include "draw_cache_impl.hh"
#include "draw_shader.hh"
#include "draw_shader_shared.hh"
#include "draw_skinning.hh"
#include "draw_skinning_defines.hh"

#include "GPU_capabilities.hh"
#include "GPU_compute.hh"
#include "GPU_uniform_buffer.hh"
#include "GPU_vertex_buffer.hh"
#include "gpu_shader_create_info.hh"

#include "mesh_extractors/extract_mesh.hh"

namespace blender::draw {

void draw_skinning_cache_free(DRWSkinningCache &cache)
{
  /*Cleanup.*/
  GPU_VERTBUF_DISCARD_SAFE(cache.in_indices_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.in_weights_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.in_vertpos_buf);
  if (cache.bonedata_mat) {
    MEM_freeN(cache.bonedata_mat);
    cache.bonedata_mat = nullptr;
  }
  // if (cache.bonedata_segments) {
  //   MEM_freeN(cache.bonedata_segments);
  //   cache.bonedata_segments = nullptr;
  // }
  // if (cache.bonedata_offsets) {
  //   MEM_freeN(cache.bonedata_offsets);
  //   cache.bonedata_offsets = nullptr;
  // }
  // if (cache.bonedata_lengths) {
  //   MEM_freeN(cache.bonedata_lengths);
  //   cache.bonedata_lengths = nullptr;
  // }
  // if (cache.bonedata_invarmmat) {
  //   MEM_freeN(cache.bonedata_invarmmat);
  //   cache.bonedata_invarmmat = nullptr;
  // }
  // if (cache.armspace_data) {
  //   MEM_freeN(cache.armspace_data);
  //   cache.armspace_data = nullptr;
  // }
  // if (cache.targspace_data) {
  //   MEM_freeN(cache.targspace_data);
  //   cache.targspace_data = nullptr;
  // }
  // if (cache.in_bonemat_buf) {
  //   GPU_storagebuf_free(cache.in_bonemat_buf);
  //   cache.in_bonemat_buf = nullptr;
  // }
  // if (cache.in_bonesegments_buf) {
  //   GPU_storagebuf_free(cache.in_bonesegments_buf);
  //   cache.in_bonesegments_buf = nullptr;
  // }
  // if (cache.in_boneoffsets_buf) {
  //   GPU_storagebuf_free(cache.in_boneoffsets_buf);
  //   cache.in_boneoffsets_buf = nullptr;
  // }
  // if (cache.in_bonelengths_buf) {
  //   GPU_storagebuf_free(cache.in_bonelengths_buf);
  //   cache.in_bonelengths_buf = nullptr;
  // }
  // if (cache.in_bone_invarmmat_buf) {
  //   GPU_storagebuf_free(cache.in_bone_invarmmat_buf);
  //   cache.in_bone_invarmmat_buf = nullptr;
  // }
  // if (cache.in_armspace_buf) {
  //   GPU_uniformbuf_free(cache.in_armspace_buf);
  //   cache.in_armspace_buf = nullptr;
  // }
  // if (cache.in_targspace_buf) {
  //   GPU_uniformbuf_free(cache.in_targspace_buf);
  //   cache.in_targspace_buf = nullptr;
  // }

  GPU_VERTBUF_DISCARD_SAFE(cache.face_adjacency_offsets_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.face_adjacency_lists_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.corner_verts_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.face_offsets_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.sharp_faces_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.vert_normals_buf);

  if (cache.original_bounds_buf) {
    GPU_storagebuf_free(cache.original_bounds_buf);
    cache.original_bounds_buf = nullptr;
  }
  if (cache.bounds_result_buf) {
    GPU_storagebuf_free(cache.bounds_result_buf);
    cache.bounds_result_buf = nullptr;
  }

  cache.influence_nums = 0;
  cache.bone_count = 0;
  cache.corner_nums = 0;
  cache.verts_num = 0;
  cache.faces_num = 0;
  cache.edges_num = 0;
  cache.cached_deform_flag = 0;
  cache.vertex_data_packed = false;
  cache.total_segments = 0;
}

void draw_free_skinning_runtime_cache(const Object &ob)
{
  LISTBASE_FOREACH (ModifierData *, md, &ob.modifiers) {
    if (md->type == eModifierType_Armature) {
      ArmatureModifierData *amd = (ArmatureModifierData *)md;
      DRWSkinningCache *cache = static_cast<DRWSkinningCache *>(amd->modifier.runtime);
      /* early out */
      if (cache == nullptr) {
        return;
      }
      else {
        draw_skinning_cache_free(*cache);
        MEM_freeN(cache);
        amd->modifier.runtime = nullptr;
      }
    }
  }
}

/* -------------------------------------------------------------------- */
/** \name Mesh extraction & Packing
 *
 * Extracts mesh data and compresses to save on GPU memory.
 * \{ */

static void draw_skinning_pack_vertex_data(Object *armature_ob,
                                           float **r_meshdata_pos,
                                           //  float **r_meshdata_nor,
                                           //  float **r_meshdata_tan,
                                           uint32_t **r_meshdata_idx,
                                           float **r_meshdata_wgt,
                                           MeshRenderData &mr)
{
  const int verts_num = mr.mesh->verts_num;
  if (verts_num == 0) {
    return;
  }

  const int total_elements = mr.corners_num + mr.loose_indices_num;

  /* Extract data per-corner but get vertex data for each corner */
  MutableSpan<float4> pos_data(reinterpret_cast<float4 *>(*r_meshdata_pos), total_elements);
  MutableSpan corners_data = pos_data.take_front(mr.corners_num);
  MutableSpan loose_edge_data = pos_data.slice(mr.corners_num, mr.loose_edges.size() * 2);
  MutableSpan loose_vert_data = pos_data.take_back(mr.loose_verts.size());

  /* Extract positions per corner */
  for (int i = 0; i < mr.corners_num; i++) {
    const float3 &pos = mr.vert_positions[mr.corner_verts[i]];
    corners_data[i] = float4(pos.x, pos.y, pos.z, 1.0f);
  }

  for (int i = 0; i < mr.loose_edges.size() * 2; i++) {
    int edge_idx = i / 2;
    int vert_in_edge = i % 2;
    int edge_index = mr.loose_edges[edge_idx];
    int vert_idx = (vert_in_edge == 0) ? mr.edges[edge_index][0] : mr.edges[edge_index][1];
    const float3 &pos = mr.vert_positions[vert_idx];
    loose_edge_data[i] = float4(pos.x, pos.y, pos.z, 1.0f);
  }

  for (int i = 0; i < mr.loose_verts.size(); i++) {
    const float3 &pos = mr.vert_positions[mr.loose_verts[i]];
    loose_vert_data[i] = float4(pos.x, pos.y, pos.z, 1.0f);
  }

  const Span<float3> vert_normals = mr.mesh->vert_normals();
  const Span<MDeformVert> dverts = mr.mesh->deform_verts();

  const ListBase *defbase = &mr.mesh->vertex_group_names;
  const int defbase_len = BLI_listbase_count(defbase);

  bPoseChannel **pchan_from_defbase = static_cast<bPoseChannel **>(
      MEM_callocN(sizeof(*pchan_from_defbase) * defbase_len, "defnrToBone"));

  int *bone_index_from_defbase = static_cast<int *>(
      MEM_mallocN(sizeof(*bone_index_from_defbase) * defbase_len, "boneIdxFromDefbase"));

  int i;
  LISTBASE_FOREACH_INDEX (bDeformGroup *, dg, defbase, i) {
    pchan_from_defbase[i] = BKE_pose_channel_find_name(armature_ob->pose, dg->name);
    bone_index_from_defbase[i] = -1; /* default to invalid */

    if (pchan_from_defbase[i]) {
      if (pchan_from_defbase[i]->bone->flag & BONE_NO_DEFORM) {
        pchan_from_defbase[i] = nullptr;
      }
      else {
        int bone_idx = 0;
        LISTBASE_FOREACH (bPoseChannel *, test_pchan, &armature_ob->pose->chanbase) {
          if (!(test_pchan->bone->flag & BONE_NO_DEFORM)) {
            if (test_pchan == pchan_from_defbase[i]) {
              bone_index_from_defbase[i] = bone_idx;
              break;
            }
            bone_idx++;
          }
        }
      }
    }
  }
#if 0
  MutableSpan<float2> nor_data(reinterpret_cast<float2 *>(*r_meshdata_nor), total_elements);

  MutableSpan corners_nor_data = nor_data.take_front(mr.corners_num);
  MutableSpan loose_edge_nor_data = nor_data.slice(mr.corners_num, mr.loose_edges.size() * 2);
  MutableSpan loose_vert_nor_data = nor_data.take_back(mr.loose_verts.size());

  /* Octahedral compression for mesh normals, helps with mem bandwidth. */
  auto encode_octahedral = [](const float3 &normal) -> float2 {
    float3 n = math::normalize(normal);
    float vx = n.x, vy = n.y, vz = n.z;

    float inv_sum = 1.0f / (fabsf(vx) + fabsf(vy) + fabsf(vz));
    float px = vx * inv_sum;
    float py = vy * inv_sum;

    if (vz <= 0.0f) {
      float oldx = px;
      px = (1.0f - fabsf(py)) * (px >= 0.0f ? 1.0f : -1.0f);
      py = (1.0f - fabsf(oldx)) * (py >= 0.0f ? 1.0f : -1.0f);
    }

    return float2(px, py);
  };

  threading::parallel_for(IndexRange(mr.corners_num), 1024, [&](IndexRange range) {
    for (int i : range) {
      int vert_idx = mr.corner_verts[i];
      float3 n(0.0f, 0.0f, 1.0f);

      if (vert_idx < vert_normals.size()) {
        n = vert_normals[vert_idx];
      }

      corners_nor_data[i] = encode_octahedral(n);
    }
  });

  for (int i = 0; i < mr.loose_edges.size() * 2; i++) {
    int edge_idx = i / 2;
    int vert_in_edge = i % 2;
    int edge_index = mr.loose_edges[edge_idx];
    int vert_idx = (vert_in_edge == 0) ? mr.edges[edge_index][0] : mr.edges[edge_index][1];

    float3 n(0.0f, 0.0f, 1.0f);
    if (vert_idx < vert_normals.size()) {
      n = vert_normals[vert_idx];
    }
    loose_edge_nor_data[i] = encode_octahedral(n);
  }

  for (int i = 0; i < mr.loose_verts.size(); i++) {
    int vert_idx = mr.loose_verts[i];
    float3 n(0.0f, 0.0f, 1.0f);
    if (vert_idx < vert_normals.size()) {
      n = vert_normals[vert_idx];
    }
    loose_vert_nor_data[i] = encode_octahedral(n);
  }

  MutableSpan<float4> tan_data(reinterpret_cast<float4 *>(*r_meshdata_tan), total_elements);

  MutableSpan corners_tan_data = tan_data.take_front(mr.corners_num);
  MutableSpan loose_edge_tan_data = tan_data.slice(mr.corners_num, mr.loose_edges.size() * 2);
  MutableSpan loose_vert_tan_data = tan_data.take_back(mr.loose_verts.size());

  /* Calculate tangents using the default UV layer */
  Array<Array<float4>> tangent_arrays;
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const StringRef default_uv_name = mr.mesh->default_uv_map_name();

  if (!default_uv_name.is_empty()) {
    VArraySpan<float2> uv_map = *attributes.lookup<float2>(default_uv_name,
                                                           bke::AttrDomain::Corner);
    Array<Span<float2>> uv_map_spans(1);
    uv_map_spans[0] = uv_map;

    tangent_arrays = bke::mesh::calc_uv_tangents(mr.vert_positions,
                                                 mr.faces,
                                                 mr.corner_verts,
                                                 mr.mesh->corner_tris(),
                                                 mr.mesh->corner_tri_faces(),
                                                 mr.sharp_faces,
                                                 mr.mesh->vert_normals(),
                                                 mr.face_normals,
                                                 mr.corner_normals,
                                                 uv_map_spans);
  }

  if (!tangent_arrays.is_empty() && !tangent_arrays[0].is_empty()) {
    const Span<float4> tangents = tangent_arrays[0];

    for (int i = 0; i < mr.corners_num; i++) {
      corners_tan_data[i] = tangents[i];
    }
  }
  else {
    for (int i = 0; i < mr.corners_num; i++) {
      corners_tan_data[i] = float4(1.0f, 0.0f, 0.0f, 1.0f);
    }
  }

  for (int i = 0; i < mr.loose_edges.size() * 2; i++) {
    loose_edge_tan_data[i] = float4(1.0f, 0.0f, 0.0f, 1.0f);
  }

  for (int i = 0; i < mr.loose_verts.size(); i++) {
    loose_vert_tan_data[i] = float4(1.0f, 0.0f, 0.0f, 1.0f);
  }
#endif

  int max_influences = U.gpuskin_influences;

  MutableSpan<uint32_t> idx_data(reinterpret_cast<uint32_t *>(*r_meshdata_idx),
                                 total_elements * max_influences);
  MutableSpan<float> wgt_data(reinterpret_cast<float *>(*r_meshdata_wgt),
                              total_elements * max_influences);

  struct Influence {
    int bone_idx;
    float weight;
  };

  auto extract_vertex_weights = [&](int vert_idx, int output_idx, int max_influences) {
    if (vert_idx >= dverts.size()) {
      int base_idx = output_idx * max_influences;
      if (base_idx + max_influences <= int(idx_data.size())) {
        for (int k = 0; k < max_influences; ++k) {
          idx_data[base_idx + k] = 0xFFFFFFFFu;
          wgt_data[base_idx + k] = 0.0f;
        }
      }
      return;
    }

    const MDeformVert &dvert = dverts[vert_idx];

    constexpr int MAX_STACK_INFLUENCES = 32;
    Influence infl_buf[MAX_STACK_INFLUENCES];
    int count = 0;

    for (int j = 0; j < dvert.totweight; ++j) {
      const uint def_nr = dvert.dw[j].def_nr;
      if (def_nr < defbase_len && bone_index_from_defbase[def_nr] >= 0) {
        const float weight = dvert.dw[j].weight;
        if (weight > 1e-5f) {
          if (count < MAX_STACK_INFLUENCES) {
            infl_buf[count].bone_idx = bone_index_from_defbase[def_nr];
            infl_buf[count].weight = weight;
            count++;
          }
        }
      }
    }

    if (count > max_influences) {
      std::partial_sort(
          infl_buf,
          infl_buf + max_influences,
          infl_buf + count,
          [](const Influence &a, const Influence &b) { return a.weight > b.weight; });
      count = max_influences;
    }
    else {
      std::sort(infl_buf, infl_buf + count, [](const Influence &a, const Influence &b) {
        return a.weight > b.weight;
      });
    }

    float total_weight = 0.0f;
    for (int i = 0; i < count; ++i) {
      total_weight += infl_buf[i].weight;
    }

    const float inv_total = (total_weight > 1e-5f) ? (1.0f / total_weight) : 0.0f;

    int base_idx = output_idx * max_influences;

    /* Sanity check */
    if (base_idx + max_influences > int(idx_data.size())) {
      return;
    }

    for (int k = 0; k < max_influences; ++k) {
      if (k < count) {
        idx_data[base_idx + k] = uint32_t(infl_buf[k].bone_idx);
        wgt_data[base_idx + k] = infl_buf[k].weight * inv_total;
      }
      else {
        /* Fill unused slots with invalid index and zero weight */
        idx_data[base_idx + k] = 0xFFFFFFFFu;
        wgt_data[base_idx + k] = 0.0f;
      }
    }
  };

  /* Extract weights per corner */

  threading::parallel_for(IndexRange(mr.corners_num), 1024, [&](IndexRange range) {
    for (int i : range) {
      int vert_idx = mr.corner_verts[i];
      extract_vertex_weights(vert_idx, i, max_influences);
    }
  });

  for (int i = 0; i < mr.loose_edges.size() * 2; i++) {
    int edge_idx = i / 2;
    int vert_in_edge = i % 2;
    int edge_index = mr.loose_edges[edge_idx];
    int vert_idx = (vert_in_edge == 0) ? mr.edges[edge_index][0] : mr.edges[edge_index][1];
    extract_vertex_weights(vert_idx, mr.corners_num + i, max_influences);
  }

  for (int i = 0; i < mr.loose_verts.size(); i++) {
    int vert_idx = mr.loose_verts[i];
    extract_vertex_weights(
        vert_idx, mr.corners_num + mr.loose_edges.size() * 2 + i, max_influences);
  }

  if (pchan_from_defbase) {
    MEM_freeN(pchan_from_defbase);
  }
  if (bone_index_from_defbase) {
    MEM_freeN(bone_index_from_defbase);
  }
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Bone matrices Extraction
 *
 * Setups bone matrices buffer for GPU skinning
 * \{ */

static int draw_get_bone_count(Object *armature_ob)
{
  bPose *pose = armature_ob->pose;
  int bone_count = 0;

  LISTBASE_FOREACH (bPoseChannel *, pchan, &pose->chanbase) {
    if (pchan->bone->flag & BONE_NO_DEFORM) {
      continue;
    }
    bone_count++;
  }
  return bone_count;
}

/* Fetching the bendy-bones segment count than bone count is more preferred
 * as we get the b-bone count directly and even if the bone doesn't have B-bone
 * it's still counted this helps build the bone matrix buffer and not have conditional logic. */
static int draw_get_segment_count(Object *armature_ob)
{
  bPose *pose = armature_ob->pose;
  int total_matrix_count = 0;

  LISTBASE_FOREACH (bPoseChannel *, pchan, &pose->chanbase) {
    if (pchan->bone->flag & BONE_NO_DEFORM) {
      continue;
    }
    int segs = pchan->bone->segments;
    total_matrix_count += (segs > 1) ? (segs + 1) : 1;
  }

  return total_matrix_count;
}

static void fill_bbone_segment_info(Object *armature_ob, DRWSkinningCache &cache)
{
  int bi = 0;
  int current_offset = 0;

  LISTBASE_FOREACH (bPoseChannel *, pchan, &armature_ob->pose->chanbase) {
    if (pchan->bone->flag & BONE_NO_DEFORM) {
      continue;
    }
    const int segments = pchan->bone->segments;
    cache.bonedata_buf[bi].segments = segments;
    cache.bonedata_buf[bi].offsets = current_offset;
    current_offset += (segments > 1) ? (segments + 1) : 1;

    bi++;
  }
}

static void draw_skinning_pack_bone_data(DRWSkinningCache &cache, Object *armature_ob)
{
  int bone_index = 0;
  LISTBASE_FOREACH (bPoseChannel *, pchan, &armature_ob->pose->chanbase) {
    if (pchan->bone->flag & BONE_NO_DEFORM) {
      continue;
    }

    cache.bonedata_buf[bone_index].lengths = pchan->bone->length;

    float inv_arm_mat[4][4];
    /*I think this is expensive especially when there's a lot of characters*/
    invert_m4_m4(inv_arm_mat, pchan->bone->arm_mat);
    memcpy(&(cache.bonedata_buf[bone_index].inverse_arm),
           &inv_arm_mat,
           sizeof(float) * 16);
    bone_index++;
  }
}

static void draw_skinning_update_bone_matrices(DRWSkinningCache &cache,
                                             Object *armature_ob,
                                             Object *target_ob)
{
  /* early out */
  if (!armature_ob || !armature_ob->pose) {
    return;
  }

  float4x4 target_to_world = target_ob->object_to_world();
  float4x4 armature_to_world = armature_ob->object_to_world();
  float4x4 world_to_target = math::invert(target_to_world);

  float4x4 armature_to_target = world_to_target * armature_to_world;

  float4x4 target_to_armature = math::invert(armature_to_target);

  cache.armature_buf->ArmatureToSpace = armature_to_target;
  cache.armature_buf->TargetToSpace = target_to_armature;

  int bone_index = 0;
  int mat_idx = 0;

  LISTBASE_FOREACH (bPoseChannel *, pchan, &armature_ob->pose->chanbase) {
    if (bone_index >= cache.bone_count) {
      break;
    }
    if (pchan->bone->flag & BONE_NO_DEFORM) {
      continue;
    }
    const int segments = int(cache.bonedata_buf[bone_index].segments);

    if (segments > 1) {
      for (int seg = 0; seg <= segments; seg++) {

        int src_idx = seg + 1;
        const Mat4 &bbone_mat = pchan->runtime.bbone_deform_mats[src_idx];

        memcpy(&(cache.bonedata_mat)[mat_idx++ * 16], &bbone_mat,
               sizeof(float) * 16);
      }
    }
    else {
      memcpy(&(cache.bonedata_mat)[mat_idx++ * 16], &pchan->chan_mat,
             sizeof(float) * 16);
    }
    bone_index++;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Setup input buffers and shader
 *
 * Setups shader and buffers for packing and upload
 * \{ */

static void draw_skinning_setup_buffers(Object *armature_ob,
                                        DRWSkinningCache *cache,
                                        MeshRenderData &mr,
                                        const ArmatureModifierData *amd)
{

  cache->bone_count = draw_get_bone_count(armature_ob);
  cache->total_segments = draw_get_segment_count(armature_ob);

  const int total_elements = mr.corners_num + mr.loose_indices_num;
  cache->corner_nums = total_elements;

  cache->edges_num = mr.edges_num;

  cache->influence_nums = U.gpuskin_influences;

  cache->in_armspace_buf = GPU_uniformbuf_create(sizeof(ArmatureSpace));
  cache->armature_buf = (ArmatureSpace *)MEM_mallocN_aligned(
        sizeof(ArmatureSpace), 16, "GPUbendybone data");

  cache->in_bonedata_buf = GPU_storagebuf_create(cache->bone_count * sizeof(BoneData));
  cache->bonedata_buf = (BoneData *)MEM_mallocN_aligned(
        sizeof(BoneData) * cache->bone_count, 16, "GPUbendybone data");

  cache->in_indices_buf = GPU_vertbuf_calloc();
  static GPUVertFormat idx_format = {0};
  if (idx_format.attr_len == 0) {
    GPU_vertformat_attr_add(&idx_format, "inidx", gpu::VertAttrType::SINT_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->in_indices_buf, idx_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->in_indices_buf, cache->corner_nums * cache->influence_nums);

  cache->in_weights_buf = GPU_vertbuf_calloc();
  static GPUVertFormat wgt_format = {0};
  if (wgt_format.attr_len == 0) {
    GPU_vertformat_attr_add(&wgt_format, "inwgt", gpu::VertAttrType::SFLOAT_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->in_weights_buf, wgt_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->in_weights_buf, cache->corner_nums * cache->influence_nums);

  cache->in_vertpos_buf = GPU_vertbuf_calloc();
  static GPUVertFormat pos_in_format = {0};
  if (pos_in_format.attr_len == 0) {
    GPU_vertformat_attr_add(&pos_in_format, "inpos", gpu::VertAttrType::SFLOAT_32_32_32_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->in_vertpos_buf, pos_in_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->in_vertpos_buf, cache->corner_nums);

#if 0
  cache->in_vertnor_buf = GPU_vertbuf_calloc();
  static GPUVertFormat nor_in_format = {0};
  if (nor_in_format.attr_len == 0) {
    GPU_vertformat_attr_add(&nor_in_format, "innor", gpu::VertAttrType::SFLOAT_32_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->in_vertnor_buf, nor_in_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->in_vertnor_buf, cache->corner_nums);

  cache->in_verttan_buf = GPU_vertbuf_calloc();
  static GPUVertFormat tan_in_format = {0};
  if (tan_in_format.attr_len == 0) {
    GPU_vertformat_attr_add(&tan_in_format, "intan", gpu::VertAttrType::SFLOAT_32_32_32_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->in_verttan_buf, tan_in_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->in_verttan_buf, cache->corner_nums);
#endif

  cache->cached_deform_flag = amd ? amd->deformflag : 0;

  cache->in_bonemat_buf = GPU_storagebuf_create(cache->total_segments * 16 * sizeof(float));
  cache->bonedata_mat = static_cast<float *>(
      MEM_mallocN(cache->total_segments * 16 * sizeof(float), "in_bonemat"));

  cache->meshdata_wgt = cache->in_weights_buf->data<float>().data();
  cache->meshdata_idx = cache->in_indices_buf->data<uint32_t>().data();
  cache->meshdata_pos = cache->in_vertpos_buf->data<float>().data();
  // cache->meshdata_nor = cache->in_vertnor_buf->data<float>().data();
  // cache->meshdata_tan = cache->in_verttan_buf->data<float>().data();

  GPU_vertbuf_tag_dirty(cache->in_weights_buf);
  GPU_vertbuf_tag_dirty(cache->in_indices_buf);
  GPU_vertbuf_tag_dirty(cache->in_vertpos_buf);
  // GPU_vertbuf_tag_dirty(cache->in_vertnor_buf);
  // GPU_vertbuf_tag_dirty(cache->in_verttan_buf);

  cache->skin_shader = DRW_shader_armature_skinning_lbs_get();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Normal Reconstruction Buffers Setup
 *
 * Builds face adjacency data for reconstructing normals from deformed positions.
 * \{ */

static void draw_skinning_setup_normal_buffers(DRWSkinningCache *cache, MeshRenderData &mr)
{
  /* TODO: support custom Normals and sharp edges*/
  const int verts_num = mr.mesh->verts_num;
  const int faces_num = mr.faces.size();
  const int corners_num = mr.corners_num;

  if (verts_num == 0 || faces_num == 0) {
    return;
  }

  cache->verts_num = verts_num;
  cache->faces_num = faces_num;

  Array<int> vert_face_counts(verts_num, 0);
  for (const int face_i : mr.faces.index_range()) {
    const IndexRange face = mr.faces[face_i];
    for (const int corner : face) {
      const int vert = mr.corner_verts[corner];
      vert_face_counts[vert]++;
    }
  }

  Array<uint32_t> adjacency_offsets(verts_num + 1);
  adjacency_offsets[0] = 0;
  for (int i = 0; i < verts_num; i++) {
    adjacency_offsets[i + 1] = adjacency_offsets[i] + vert_face_counts[i];
  }
  const int total_adjacency = adjacency_offsets[verts_num];

  Array<uint32_t> adjacency_lists(total_adjacency);
  Array<int> vert_current_offset(verts_num, 0);

  for (const int face_i : mr.faces.index_range()) {
    const IndexRange face = mr.faces[face_i];
    const uint32_t face_start = uint32_t(face.start());
    const uint32_t face_size = uint32_t(face.size());
    const uint32_t packed_face_info = face_start | (face_size << 24);

    for (const int corner : face) {
      const int vert = mr.corner_verts[corner];
      const int offset = adjacency_offsets[vert] + vert_current_offset[vert];
      adjacency_lists[offset] = packed_face_info;
      vert_current_offset[vert]++;
    }
  }

  Array<uint32_t> corner_verts_data(corners_num);
  for (int i = 0; i < corners_num; i++) {
    corner_verts_data[i] = uint32_t(mr.corner_verts[i]);
  }

  Array<uint32_t> face_offsets_data(faces_num + 1);
  for (const int face_i : mr.faces.index_range()) {
    face_offsets_data[face_i] = uint32_t(mr.faces[face_i].start());
  }
  face_offsets_data[faces_num] = uint32_t(corners_num);

  const int sharp_faces_words = (faces_num + 31) / 32;
  Array<uint32_t> sharp_faces_data(sharp_faces_words, 0);

  if (!mr.sharp_faces.is_empty()) {
    for (int face_i = 0; face_i < faces_num; face_i++) {
      if (mr.sharp_faces[face_i]) {
        const int word_idx = face_i / 32;
        const int bit_idx = face_i % 32;
        sharp_faces_data[word_idx] |= (1u << bit_idx);
      }
    }
  }

  /* Face adjacency offsets buffer */
  cache->face_adjacency_offsets_buf = GPU_vertbuf_calloc();
  static GPUVertFormat offset_format = {0};
  if (offset_format.attr_len == 0) {
    GPU_vertformat_attr_add(&offset_format, "offset", gpu::VertAttrType::UINT_32);
  }
  GPU_vertbuf_init_with_format_ex(
      *cache->face_adjacency_offsets_buf, offset_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->face_adjacency_offsets_buf, verts_num + 1);
  memcpy(cache->face_adjacency_offsets_buf->data<uint32_t>().data(),
         adjacency_offsets.data(),
         (verts_num + 1) * sizeof(uint32_t));
  GPU_vertbuf_tag_dirty(cache->face_adjacency_offsets_buf);

  /* Face adjacency lists buffer */
  cache->face_adjacency_lists_buf = GPU_vertbuf_calloc();
  static GPUVertFormat list_format = {0};
  if (list_format.attr_len == 0) {
    GPU_vertformat_attr_add(&list_format, "face_info", gpu::VertAttrType::UINT_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->face_adjacency_lists_buf, list_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->face_adjacency_lists_buf, total_adjacency);
  memcpy(cache->face_adjacency_lists_buf->data<uint32_t>().data(),
         adjacency_lists.data(),
         total_adjacency * sizeof(uint32_t));
  GPU_vertbuf_tag_dirty(cache->face_adjacency_lists_buf);

  /* Corner verts buffer */
  cache->corner_verts_buf = GPU_vertbuf_calloc();
  static GPUVertFormat corner_format = {0};
  if (corner_format.attr_len == 0) {
    GPU_vertformat_attr_add(&corner_format, "vert_idx", gpu::VertAttrType::UINT_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->corner_verts_buf, corner_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->corner_verts_buf, corners_num);
  memcpy(cache->corner_verts_buf->data<uint32_t>().data(),
         corner_verts_data.data(),
         corners_num * sizeof(uint32_t));
  GPU_vertbuf_tag_dirty(cache->corner_verts_buf);

  /* Face offsets buffer (maps face index to start corner) */
  cache->face_offsets_buf = GPU_vertbuf_calloc();
  static GPUVertFormat face_offset_format = {0};
  if (face_offset_format.attr_len == 0) {
    GPU_vertformat_attr_add(&face_offset_format, "face_start", gpu::VertAttrType::UINT_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->face_offsets_buf, face_offset_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->face_offsets_buf, faces_num + 1);
  memcpy(cache->face_offsets_buf->data<uint32_t>().data(),
         face_offsets_data.data(),
         (faces_num + 1) * sizeof(uint32_t));
  GPU_vertbuf_tag_dirty(cache->face_offsets_buf);

  /* Sharp faces buffer */
  cache->sharp_faces_buf = GPU_vertbuf_calloc();
  static GPUVertFormat sharp_format = {0};
  if (sharp_format.attr_len == 0) {
    GPU_vertformat_attr_add(&sharp_format, "sharp_bits", gpu::VertAttrType::UINT_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->sharp_faces_buf, sharp_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->sharp_faces_buf, sharp_faces_words);
  memcpy(cache->sharp_faces_buf->data<uint32_t>().data(),
         sharp_faces_data.data(),
         sharp_faces_words * sizeof(uint32_t));
  GPU_vertbuf_tag_dirty(cache->sharp_faces_buf);

  /* Vertex normals buffer output from accumulate pass */
  cache->vert_normals_buf = GPU_vertbuf_calloc();
  static GPUVertFormat vnor_format = {0};
  if (vnor_format.attr_len == 0) {
    GPU_vertformat_attr_add(&vnor_format, "normal", gpu::VertAttrType::SFLOAT_32_32_32_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->vert_normals_buf, vnor_format, GPU_USAGE_DEVICE_ONLY);
  GPU_vertbuf_data_alloc(*cache->vert_normals_buf, verts_num);

  cache->normals_accumulate_shader = DRW_shader_armature_skinning_normals_accumulate_get();
  cache->normals_finalize_shader = DRW_shader_armature_skinning_normals_finalize_get();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Skinning Shader binding
 *
 * Setup shader buffers for packing and upload
 * \{ */

void draw_skinning_accumulate_normals(gpu::VertBuf *vbo_pos,
                                      gpu::VertBuf *vbo_nor,
                                      const DRWSkinningCache &cache)
{
  GPU_shader_bind(cache.normals_accumulate_shader);

  /* inputs */
  GPU_vertbuf_bind_as_ssbo(vbo_pos, NORMALS_ACCUM_SKINNED_POS_BUF_SLOT);

  GPU_vertbuf_bind_as_ssbo(cache.face_adjacency_offsets_buf,
                           NORMALS_ACCUM_FACE_ADJACENCY_OFFSETS_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(cache.face_adjacency_lists_buf,
                           NORMALS_ACCUM_FACE_ADJACENCY_LISTS_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(cache.corner_verts_buf, NORMALS_ACCUM_CORNER_VERTS_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(cache.vert_normals_buf, NORMALS_ACCUM_VERT_NORMALS_BUF_SLOT);

  GPU_shader_uniform_1i(cache.normals_accumulate_shader, "vertex_count", cache.verts_num);

  const int accumulate_workgroups = divide_ceil_u(cache.verts_num, SKINNING_LOCAL_SIZE);
  GPU_compute_dispatch(cache.normals_accumulate_shader, accumulate_workgroups, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY);

  GPU_shader_unbind();
}

void draw_skinning_finalize_normals(gpu::VertBuf *vbo_pos,
                                    gpu::VertBuf *vbo_nor,
                                    const DRWSkinningCache &cache)
{
  GPU_shader_bind(cache.normals_finalize_shader);

  GPU_vertbuf_bind_as_ssbo(vbo_pos, NORMALS_FINAL_SKINNED_POS_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(cache.vert_normals_buf, NORMALS_FINAL_VERT_NORMALS_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(cache.corner_verts_buf, NORMALS_FINAL_CORNER_VERTS_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(cache.face_offsets_buf, NORMALS_FINAL_FACE_OFFSETS_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(cache.sharp_faces_buf, NORMALS_FINAL_SHARP_FACES_BUF_SLOT);
  /* output */
  GPU_vertbuf_bind_as_ssbo(vbo_nor, NORMALS_FINAL_OUT_SKINNED_NOR_BUF_SLOT);

  GPU_shader_uniform_1i(cache.normals_finalize_shader, "face_count", cache.faces_num);

  const int workgroups = divide_ceil_u(cache.faces_num, SKINNING_LOCAL_SIZE);
  GPU_compute_dispatch(cache.normals_finalize_shader, workgroups, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY);

  GPU_shader_unbind();
}

void draw_skinning_compute_position(gpu::VertBuf *vbo_pos_output,
                                    // gpu::VertBuf *vbo_nor_output,
                                    // gpu::VertBuf *vbo_tan_output,
                                    const DRWSkinningCache &cache)
{
  GPU_shader_bind(cache.skin_shader);

  /* inputs */
  // GPU_uniformbuf_bind(cache.in_targspace_buf, LBS_BONE_TARGET_TOSPACE_BUF_SLOT);

  GPU_vertbuf_bind_as_ssbo(cache.in_indices_buf, 0);
  GPU_vertbuf_bind_as_ssbo(cache.in_weights_buf, 1);
  GPU_storagebuf_bind(cache.in_bonemat_buf, 2);
  GPU_uniformbuf_bind(cache.in_armspace_buf, 3);
  GPU_storagebuf_bind(cache.in_bonedata_buf, 4);

  GPU_vertbuf_bind_as_ssbo(cache.in_vertpos_buf, 5);
  GPU_vertbuf_bind_as_ssbo(vbo_pos_output, 6);
/* Unused for now, might be used again: we could have two performance modes for the user to choose
 * between properly recomputed normals or directly skinning normals...*/
#if 0
  GPU_vertbuf_bind_as_ssbo(cache.in_vertnor_buf, LBS_VERT_NOR_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(cache.in_verttan_buf, LBS_VERT_TAN_BUF_SLOT);

  GPU_vertbuf_bind_as_ssbo(vbo_nor_output, LBS_SKINNED_NOR_BUF_SLOT);
  GPU_vertbuf_bind_as_ssbo(vbo_tan_output, LBS_SKINNED_TAN_BUF_SLOT);
#endif

  // GPU_storagebuf_bind(cache.in_bonesegments_buf, LBS_BONE_SEGMENTS_BUF_SLOT);
  // GPU_storagebuf_bind(cache.in_boneoffsets_buf, LBS_BONE_OFFSETS_BUF_SLOT);
  // GPU_storagebuf_bind(cache.in_bonelengths_buf, LBS_BONE_LENGTHS_BUF_SLOT);
  // GPU_storagebuf_bind(cache.in_bone_invarmmat_buf, LBS_BONE_INVARMMAT_BUF_SLOT);

  GPU_shader_uniform_1i(cache.skin_shader, "vertex_count", cache.corner_nums);
  GPU_shader_uniform_1i(cache.skin_shader, "influence_count", cache.influence_nums);

  const int workgroups = divide_ceil_u(cache.corner_nums, SKINNING_LOCAL_SIZE);
  GPU_compute_dispatch(cache.skin_shader, workgroups, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY |
                     GPU_BARRIER_UNIFORM);

  /* Cleanup. */
  GPU_shader_unbind();
}

void draw_skinning_compute_bounds(Mesh *mesh,
                                  DRWSkinningCache &cache,
                                  gpu::VertBuf *skinned_positions_vbo)
{
  /* Kinda horrible code that reinterpret cast and SSBO read is nasty, but this is temporary,
   * improve later with some precomputed method...*/
  auto original_bounds = mesh->bounds_min_max();
  if (!original_bounds) {
    return;
  }

  gpu::Shader *aabb_shader = DRW_shader_armature_skinning_aabb_get();

  if (!cache.bounds_result_buf) {
    cache.bounds_result_buf = GPU_storagebuf_create(6 * sizeof(uint32_t));
  }

  if (!cache.original_bounds_buf) {
    cache.original_bounds_buf = GPU_storagebuf_create(2 * sizeof(float4));
  }

  float4 bounds_data[2];
  bounds_data[0] = float4(original_bounds->min, 1.0f);
  bounds_data[1] = float4(original_bounds->max, 1.0f);
  GPU_storagebuf_update(cache.original_bounds_buf, bounds_data);

  auto float_to_sortable_uint = [](float f) -> uint32_t {
    uint32_t u = *reinterpret_cast<uint32_t *>(&f);
    return (u & 0x80000000u) != 0u ? ~u : (u | 0x80000000u);
  };

  uint32_t init_bounds[6] = {
      float_to_sortable_uint(1e30f),  /* min.x */
      float_to_sortable_uint(1e30f),  /* min.y */
      float_to_sortable_uint(1e30f),  /* min.z */
      float_to_sortable_uint(-1e30f), /* max.x */
      float_to_sortable_uint(-1e30f), /* max.y */
      float_to_sortable_uint(-1e30f), /* max.z */
  };
  GPU_storagebuf_update(cache.bounds_result_buf, init_bounds);

  GPU_shader_bind(aabb_shader);

  GPU_vertbuf_bind_as_ssbo(skinned_positions_vbo, 0);
  GPU_storagebuf_bind(cache.original_bounds_buf, 1);
  GPU_storagebuf_bind(cache.bounds_result_buf, 2);

  GPU_shader_uniform_1i(aabb_shader, "vertex_count_aabb", cache.corner_nums);

  const int workgroups = divide_ceil_u(cache.corner_nums, SKINNING_AABB_LOCAL_SIZE);
  GPU_compute_dispatch(aabb_shader, workgroups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY);

  GPU_shader_unbind();

  uint32_t result_data[6];
  GPU_storagebuf_read(cache.bounds_result_buf, result_data);

  auto sortable_uint_to_float = [](uint32_t u) -> float {
    /* Convert sortable uint back to float */
    uint32_t f_bits = (u & 0x80000000u) != 0u ? (u & 0x7FFFFFFFu) : ~u;
    return *reinterpret_cast<float *>(&f_bits);
  };

  float3 computed_min(sortable_uint_to_float(result_data[0]),
                      sortable_uint_to_float(result_data[1]),
                      sortable_uint_to_float(result_data[2]));

  float3 computed_max(sortable_uint_to_float(result_data[3]),
                      sortable_uint_to_float(result_data[4]),
                      sortable_uint_to_float(result_data[5]));
  Bounds<float3> object_space_bounds(computed_min, computed_max);

  mesh->runtime->bounds_cache.tag_dirty();
  mesh->runtime->bounds_cache.ensure(
      [&object_space_bounds](Bounds<float3> &r_data) { r_data = object_space_bounds; });
}



/** \} */

static bool draw_is_skinning_dirty(const Mesh &mesh, const DRWSkinningCache &cache)
{
  if (cache.influence_nums != U.gpuskin_influences) {
    return true;
  }
  if (cache.corner_nums != mesh.corners_num || cache.faces_num != mesh.faces_num ||
      cache.verts_num != mesh.verts_num || cache.edges_num != mesh.edges_num)
  {
    return true;
  }
  else {
    return false;
  }
}

/* -------------------------------------------------------------------- */
/** \name Main skinning creation runtime
 * \{ */

static void draw_create_skinning(Object &ob,
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
                                 const bool use_hide)
{

  LISTBASE_FOREACH (ModifierData *, md, &ob.modifiers) {
    if (md->type == eModifierType_Armature) {

      ArmatureModifierData *amd = (ArmatureModifierData *)md;
      if (!amd->object || amd->object->type != OB_ARMATURE) {
        return; /* No valid armature assigned */
      }
      /* Allocate skinning cache on modifier runtime */
      DRWSkinningCache *skincache = static_cast<DRWSkinningCache *>(amd->modifier.runtime);
      if (!skincache) {
        skincache = static_cast<DRWSkinningCache *>(
            MEM_callocN(sizeof(DRWSkinningCache), "DRWSkinningCache"));
        amd->modifier.runtime = skincache;
      }

      MeshRenderData mr = mesh_render_data_create(
          ob, mesh, is_editmode, is_paint_mode, do_final, do_uvedit, use_hide, ts);

      if (skincache) {

        bool flag_changed = (skincache->cached_deform_flag != amd->deformflag);

        // TODO (Ayoub Zouad): needs better evaluation for if topo/new modifiers added
        // TODO (Ayoub Zouad): we need to handle multimodifiers
        int unique_count = draw_get_segment_count(amd->object);
        if (flag_changed || skincache->total_segments != unique_count /*|| check mesh/modifier stack updated and or if the mesh's resting data is actively changing*/)
        {
          draw_skinning_cache_free(*skincache);
          draw_skinning_setup_buffers(amd->object, skincache, mr, amd);

          if (!skincache->vertex_data_packed) {

            fill_bbone_segment_info(amd->object, *skincache);

            draw_skinning_pack_bone_data(*skincache, amd->object);
            GPU_storagebuf_update(skincache->in_bonedata_buf, skincache->bonedata_buf);

            /* This expensive operation only needs to run once per cache lifetime */
            draw_skinning_pack_vertex_data(amd->object,
                                           &skincache->meshdata_pos,
                                           // &skincache->meshdata_nor,
                                           // &skincache->meshdata_tan,
                                           &skincache->meshdata_idx,
                                           &skincache->meshdata_wgt,
                                           mr);

            draw_skinning_setup_normal_buffers(skincache, mr);

            skincache->vertex_data_packed = true;
          }
        }

        if (draw_is_skinning_dirty(mesh, *skincache)) {
          draw_free_skinning_runtime_cache(ob);
          return;
        }

        /* Update bone matrices and re-upload buffers if cache is valid and prepared. */
        if (amd->object && amd->object->pose && skincache->bonedata_mat) {

          draw_skinning_update_bone_matrices(*skincache, amd->object, &ob);
          GPU_storagebuf_update(skincache->in_bonemat_buf, skincache->bonedata_mat);
          GPU_uniformbuf_update(skincache->in_armspace_buf, skincache->armature_buf);
        }
        /* Only create skinning buffers if skinning is prepared and valid */
        mesh_buffer_cache_create_requested_skinning(
            cache, mbc, ibo_requests, vbo_requests, *skincache, mr);
      }
      break;
    }
  }
  return;
}

void DRW_create_skinning(Object &ob,
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
                         const bool use_hide)
{
  draw_create_skinning(ob,
                       mesh,
                       cache,
                       mbc,
                       ibo_requests,
                       vbo_requests,
                       is_editmode,
                       is_paint_mode,
                       do_final,
                       do_uvedit,
                       do_cage,
                       ts,
                       use_hide);
}
/** \} */

}  // namespace blender::draw
