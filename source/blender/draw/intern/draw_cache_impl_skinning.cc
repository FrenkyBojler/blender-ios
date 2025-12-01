/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief GPU Acceleration for Armature modifier and Shape keys
 */
#include "BKE_armature.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_action.hh"
#include "BKE_mesh_tangent.hh"

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_array_utils.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_task.hh"

#include "DNA_armature_types.h"
#include "DNA_vec_types.h"
#include "DNA_armature_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_userdef_types.h"

#include "draw_cache_impl.hh"
#include "draw_defines.hh"
#include "draw_shader.hh"
#include "draw_skinning.hh"
#include "draw_cache_extract.hh"
#include "draw_shader_shared.hh"

#include "GPU_compute.hh"
#include "GPU_vertex_buffer.hh"

#include "gpu_shader_create_info.hh"

#include "mesh_extractors/extract_mesh.hh"

// using namespace blender::gpu;
// using namespace blender::gpu::shader;

namespace blender::draw {

/* We want to check if user's system supports gpu skinning */
bool draw_skinning_is_available(const Object *ob)
{
  for (ModifierData *md = (ModifierData *)ob->modifiers.first; md; md = md->next) {
    if (md->type == eModifierType_Armature) {
      ArmatureModifierData *amd = (ArmatureModifierData *)md;

      return (gpu::GCaps.max_work_group_count[0] > 0 &&
              gpu::GCaps.max_shader_storage_buffer_bindings >= 6 && (U.gpu_flag & USER_GPU_FLAG_SUBDIVISION_EVALUATION) == 1);
    }
  }
  return false;
}

void draw_skinning_cache_free(DRWSkinningCache &cache)
{
  GPU_VERTBUF_DISCARD_SAFE(cache.in_indices_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.in_weights_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.in_bonemat_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.in_vertpos_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.in_vertnor_buf);
  GPU_VERTBUF_DISCARD_SAFE(cache.in_verttan_buf);

  if (cache.in_bonedq_buf) {
    GPU_storagebuf_free(cache.in_bonedq_buf);
    cache.in_bonedq_buf = nullptr;
  }
  if (cache.bonedata_dq) {
    MEM_freeN(cache.bonedata_dq);
    cache.bonedata_dq = nullptr;
  }

  cache.bone_count = 0;
  cache.corner_nums = 0;
  cache.buffers_valid = false;

  cache.vertex_data_packed = false;
  cache.cached_deform_flag = 0;
}

/* We don't want to work with the meshcache method, because it actively deletes skincache on mesh
 * update. And we don't want to always keep running the packing on mesh eval that's too expensive,
 * so we keep the cache and delete it once we have no use for it...*/

/* TODO (Ayoub Zouad): Investigate further a better way of caching than global caching...*/
static std::unordered_map<void *, DRWSkinningCache *> g_persistent_skinning_caches;

std::unordered_map<void *, DRWSkinningCache *> &get_persistent_skinning_caches()
{
  return g_persistent_skinning_caches;
}

void draw_skinning_cache_free_object(Object *object)
{
  auto it = g_persistent_skinning_caches.find(object);
  if (it != g_persistent_skinning_caches.end()) {
    draw_skinning_cache_free(*it->second);
    MEM_delete(it->second);
    g_persistent_skinning_caches.erase(it);
  }
}

static DRWSkinningCache &mesh_batch_cache_ensure_skinning_cache(MeshBatchCache &mbc,
                                                                Object *object)
{
  auto &persistent_caches = get_persistent_skinning_caches();

  auto it = persistent_caches.find(object);
  if (it != persistent_caches.end()) {
    mbc.skinning_cache = it->second;
    return *it->second;
  }

  DRWSkinningCache *skinning_cache = MEM_new<DRWSkinningCache>(__func__);
  memset(skinning_cache, 0, sizeof(DRWSkinningCache));

  persistent_caches[object] = skinning_cache;
  mbc.skinning_cache = skinning_cache;

  return *skinning_cache;
}

/* !Idea! (Ayoub Zouad): Another idea we can implement is give the user an option to create an
 * "optimized" mesh by automatically creating new sets of the mesh's LOD via using the Decimation
 * MOD...*/

/* -------------------------------------------------------------------- */
/** \name Mesh extraction & Packing
 *
 * Extracts mesh data and compresses to save on GPU memory.
 * \{ */

static void draw_skinning_pack_vertex_data(Object *armature_ob,
                                           float **r_meshdata_pos,
                                           float **r_meshdata_nor,
                                           float **r_meshdata_tan,
                                           uint32_t **r_meshdata_idx,
                                           uint32_t **r_meshdata_wgt,
                                           MeshRenderData &mr)
{
  const int verts_num = mr.mesh->verts_num;
  if (verts_num == 0) {
    return;
  }

  const int total_elements = mr.corners_num + mr.loose_indices_num;

  /* Extract data per-corner but get vertex data for each corner */
  MutableSpan<float4> pos_data(
      reinterpret_cast<float4 *>(*r_meshdata_pos), total_elements);
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
          if (test_pchan == pchan_from_defbase[i]) {
            bone_index_from_defbase[i] = bone_idx;
            break;
          }
          bone_idx++;
        }
      }
    }
  }

  MutableSpan<float2> nor_data(
      reinterpret_cast<float2 *>(*r_meshdata_nor), total_elements);

  MutableSpan corners_nor_data = nor_data.take_front(mr.corners_num);
  MutableSpan loose_edge_nor_data = nor_data.slice(mr.corners_num,
                                                            mr.loose_edges.size() * 2);
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

  MutableSpan<float4> tan_data(
      reinterpret_cast<float4 *>(*r_meshdata_tan), total_elements);

  MutableSpan corners_tan_data = tan_data.take_front(mr.corners_num);
  MutableSpan loose_edge_tan_data = tan_data.slice(mr.corners_num,
                                                            mr.loose_edges.size() * 2);
  MutableSpan loose_vert_tan_data = tan_data.take_back(mr.loose_verts.size());

  /* Calculate tangents using the default UV layer */
  Array<Array<float4>> tangent_arrays;
  const bke::AttributeAccessor attributes = mr.mesh->attributes();
  const StringRef default_uv_name = mr.mesh->default_uv_map_name();

  if (!default_uv_name.is_empty()) {
    VArraySpan<float2> uv_map = *attributes.lookup<float2>(
        default_uv_name, bke::AttrDomain::Corner);
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

  MutableSpan<uint2> idx_data(
      reinterpret_cast<uint2 *>(*r_meshdata_idx), total_elements);

  MutableSpan<uint2> wgt_data(
      reinterpret_cast<uint2 *>(*r_meshdata_wgt), total_elements);

  struct Influence {
    int bone_idx;
    float weight;
  };
  auto insert_top4 = [](Influence buf[4], int &count, int bone, float weight) {
    if (weight <= 0.0f)
      return;
    int pos = count;
    for (int k = 0; k < count; ++k) {
      if (weight > buf[k].weight) {
        pos = k;
        break;
      }
    }
    if (pos == 4) {
      if (count < 4)
        pos = count;
      else
        return;
    }
    if (count < 4)
      ++count;
    for (int s = count - 1; s > pos; --s) {
      buf[s] = buf[s - 1];
    }
    buf[pos].bone_idx = bone;
    buf[pos].weight = weight;
  };

  auto extract_vertex_weights = [&](int vert_idx, int output_idx) {
    Influence infl_buf[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    int infl_count = 0;

    if (vert_idx < dverts.size()) {
      const MDeformVert &dvert = dverts[vert_idx];
      for (int j = 0; j < dvert.totweight; ++j) {
        const uint def_nr = dvert.dw[j].def_nr;
        if (def_nr < defbase_len && bone_index_from_defbase[def_nr] >= 0) {
          const int mapped_bone = bone_index_from_defbase[def_nr];
          insert_top4(infl_buf, infl_count, mapped_bone, dvert.dw[j].weight);
        }
      }
    }

    if (infl_count > 1) {
      std::sort(infl_buf, infl_buf + infl_count, [](const Influence &a, const Influence &b) {
        return a.weight > b.weight;
      });
    }

    float total = 0.0f;
    for (int k = 0; k < infl_count; ++k)
      total += infl_buf[k].weight;
    if (total > 0.0f) {
      float inv_total = 1.0f / total;
      for (int k = 0; k < infl_count; ++k)
        infl_buf[k].weight *= inv_total;
    }

    uint32_t i0 = (infl_count > 0) ? uint32_t(infl_buf[0].bone_idx) : 0xFFFFu;
    uint32_t i1 = (infl_count > 1) ? uint32_t(infl_buf[1].bone_idx) : 0xFFFFu;
    uint32_t i2 = (infl_count > 2) ? uint32_t(infl_buf[2].bone_idx) : 0xFFFFu;
    uint32_t i3 = (infl_count > 3) ? uint32_t(infl_buf[3].bone_idx) : 0xFFFFu;

    auto to16_unclamped = [](float w) -> uint32_t {
      float cw = fmaxf(0.0f, fminf(1.0f, w));
      return uint32_t(roundf(cw * 65535.0f));
    };

    uint32_t q[4] = {0, 0, 0, 0};
    q[0] = (infl_count > 0) ? to16_unclamped(infl_buf[0].weight) : 0u;
    q[1] = (infl_count > 1) ? to16_unclamped(infl_buf[1].weight) : 0u;
    q[2] = (infl_count > 2) ? to16_unclamped(infl_buf[2].weight) : 0u;
    q[3] = (infl_count > 3) ? to16_unclamped(infl_buf[3].weight) : 0u;

    uint32_t sumQ = q[0] + q[1] + q[2] + q[3];
    const int TARGET = 65535;
    if (sumQ != TARGET && sumQ > 0) {
      int max_idx = 0;
      for (int k = 1; k < 4; ++k)
        if (q[k] > q[max_idx])
          max_idx = k;
      int delta = TARGET - (int)sumQ;
      int64_t adjusted = int64_t(q[max_idx]) + int64_t(delta);
      if (adjusted < 0)
        adjusted = 0;
      if (adjusted > 65535)
        adjusted = 65535;
      q[max_idx] = uint32_t(adjusted);
    }

    uint32_t idx0 = (i0 & 0xFFFFu) | ((i1 & 0xFFFFu) << 16);
    uint32_t idx1 = (i2 & 0xFFFFu) | ((i3 & 0xFFFFu) << 16);
    uint32_t w0u = (q[0] & 0xFFFFu) | ((q[1] & 0xFFFFu) << 16);
    uint32_t w1u = (q[2] & 0xFFFFu) | ((q[3] & 0xFFFFu) << 16);

    idx_data[output_idx] = uint2(idx0, idx1);
    wgt_data[output_idx] = uint2(w0u, w1u);
  };

  /* Extract weights per corner */
  threading::parallel_for(IndexRange(mr.corners_num), 1024, [&](IndexRange range) {
    for (int i : range) {
      int vert_idx = mr.corner_verts[i];
      extract_vertex_weights(vert_idx, i);
    }
  });

  for (int i = 0; i < mr.loose_edges.size() * 2; i++) {
    int edge_idx = i / 2;
    int vert_in_edge = i % 2;
    int edge_index = mr.loose_edges[edge_idx];
    int vert_idx = (vert_in_edge == 0) ? mr.edges[edge_index][0] : mr.edges[edge_index][1];
    extract_vertex_weights(vert_idx, mr.corners_num + i);
  }

  for (int i = 0; i < mr.loose_verts.size(); i++) {
    int vert_idx = mr.loose_verts[i];
    extract_vertex_weights(vert_idx, mr.corners_num + mr.loose_edges.size() * 2 + i);
  }

  if (pchan_from_defbase) {
    MEM_freeN(pchan_from_defbase);
  }
  if (bone_index_from_defbase) {
    MEM_freeN(bone_index_from_defbase);
  }
}

static int draw_get_bone_count(Object *armature_ob, int *bone_count)
{
  bPose *pose = armature_ob->pose;
  *bone_count = BLI_listbase_count(&pose->chanbase);
  return *bone_count;
}

static void draw_skinning_pack_bone_matrices(Object *armature_ob,
                                             Object *target_ob,
                                             float **bonedata_mat,
                                             GPUDualQuat **bonedata_dq,
                                             int *bone_count,
                                             const bool use_dual_quaternion)
{
  if (!armature_ob || armature_ob->type != OB_ARMATURE || !armature_ob->pose) {
    return;
  }

  bPose *pose = armature_ob->pose;
  *bone_count = draw_get_bone_count(armature_ob, bone_count);

  float4x4 armature_to_target = target_ob->world_to_object() * armature_ob->object_to_world();
  float4x4 target_to_armature = math::invert(armature_to_target);

  int bone_index = 0;
  LISTBASE_FOREACH (bPoseChannel *, pchan, &pose->chanbase) {
    if (bone_index >= *bone_count)
      break;

    if (use_dual_quaternion && bonedata_dq) {
      const DualQuat &src_dq = pchan->runtime.deform_dual_quat;

      DualQuat transformed_dq;

      DualQuat armature_to_target_dq, target_to_armature_dq;
      float4x4 identity = float4x4::identity();
      mat4_to_dquat(&armature_to_target_dq, identity.ptr(), armature_to_target.ptr());
      mat4_to_dquat(&target_to_armature_dq, identity.ptr(), target_to_armature.ptr());

      float4x4 src_matrix;
      dquat_to_mat4(src_matrix.ptr(), &src_dq);
      float4x4 final_matrix = armature_to_target * src_matrix * target_to_armature;
      mat4_to_dquat(&transformed_dq, identity.ptr(), final_matrix.ptr());

      GPUDualQuat &dst_dq = (*bonedata_dq)[bone_index];

      for (int i = 0; i < 4; i++) {
        dst_dq.quat[i] = transformed_dq.quat[i];
        dst_dq.trans[i] = transformed_dq.trans[i];
      }
      for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
          dst_dq.scale[i][j] = transformed_dq.scale[i][j];
        }
      }
      dst_dq.scale_weight = transformed_dq.scale_weight;
    }
    else if (bonedata_mat) {
      float4x4 chan_mat = float4x4(pchan->chan_mat);
      float4x4 final_mat = armature_to_target * chan_mat * target_to_armature;

      const float *mat_ptr = final_mat.base_ptr();
      for (int i = 0; i < 16; i++) {
        (*bonedata_mat)[bone_index * 16 + i] = mat_ptr[i];
      }
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

  cache->bone_count = draw_get_bone_count(armature_ob, &cache->bone_count);

  const int total_elements = mr.corners_num + mr.loose_indices_num;
  cache->corner_nums = total_elements;

  cache->in_indices_buf = GPU_vertbuf_calloc();
  static GPUVertFormat idx_format = {0};
  if (idx_format.attr_len == 0) {
    GPU_vertformat_attr_add(&idx_format, "inidx", gpu::VertAttrType::UINT_32_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->in_indices_buf, idx_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->in_indices_buf, cache->corner_nums);

  cache->in_weights_buf = GPU_vertbuf_calloc();
  static GPUVertFormat wgt_format = {0};
  if (wgt_format.attr_len == 0) {
    GPU_vertformat_attr_add(&wgt_format, "inwgt", gpu::VertAttrType::UINT_32_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->in_weights_buf, wgt_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->in_weights_buf, cache->corner_nums);

  cache->in_vertpos_buf = GPU_vertbuf_calloc();
  static GPUVertFormat pos_in_format = {0};
  if (pos_in_format.attr_len == 0) {
    GPU_vertformat_attr_add(&pos_in_format, "inpos", gpu::VertAttrType::SFLOAT_32_32_32_32);
  }
  GPU_vertbuf_init_with_format_ex(*cache->in_vertpos_buf, pos_in_format, GPU_USAGE_STATIC);
  GPU_vertbuf_data_alloc(*cache->in_vertpos_buf, cache->corner_nums + 1);

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

  bool use_dual_quaternion = (amd && (amd->deformflag & ARM_DEF_QUATERNION));

  if (use_dual_quaternion) {
    cache->in_bonedq_buf = GPU_storagebuf_create(sizeof(DualQuat) * (cache->bone_count + 5));
    cache->bonedata_dq = (GPUDualQuat *)MEM_mallocN_aligned(
        sizeof(GPUDualQuat) * (cache->bone_count + 5), 16, "GPUDualQuat bone data");

    cache->in_bonemat_buf = nullptr;
    cache->bonedata_mat = nullptr;
  }
  else {
    cache->in_bonemat_buf = GPU_vertbuf_calloc();
    static GPUVertFormat bone_mat_format = {0};
    if (bone_mat_format.attr_len == 0) {
      GPU_vertformat_attr_add_legacy(
          &bone_mat_format, "inbone_mat", GPU_COMP_F32, 16, GPU_FETCH_FLOAT);
    }
    GPU_vertbuf_init_with_format_ex(*cache->in_bonemat_buf, bone_mat_format, GPU_USAGE_DYNAMIC);
    GPU_vertbuf_data_alloc(*cache->in_bonemat_buf, cache->bone_count + 1);

    cache->bonedata_mat = cache->in_bonemat_buf->data<float>().data();

    cache->in_bonedq_buf = nullptr;
    cache->bonedata_dq = nullptr;
  }

  cache->meshdata_wgt = cache->in_weights_buf->data<uint32_t>().data();
  cache->meshdata_idx = cache->in_indices_buf->data<uint32_t>().data();
  cache->meshdata_pos = cache->in_vertpos_buf->data<float>().data();
  cache->meshdata_nor = cache->in_vertnor_buf->data<float>().data();
  cache->meshdata_tan = cache->in_verttan_buf->data<float>().data();

  GPU_vertbuf_tag_dirty(cache->in_weights_buf);
  GPU_vertbuf_tag_dirty(cache->in_indices_buf);
  GPU_vertbuf_tag_dirty(cache->in_vertpos_buf);
  GPU_vertbuf_tag_dirty(cache->in_vertnor_buf);
  GPU_vertbuf_tag_dirty(cache->in_verttan_buf);

  if (cache->in_bonemat_buf) {
    GPU_vertbuf_tag_dirty(cache->in_bonemat_buf);
  }

  if (use_dual_quaternion) {
    cache->compute_shader = DRW_shader_armature_skinning_dqs_get();
  }
  else {
    cache->compute_shader = DRW_shader_armature_skinning_lbs_get();
  }

  cache->cached_deform_flag = amd ? amd->deformflag : 0;

  bool success = (cache->compute_shader != nullptr && cache->in_indices_buf != nullptr &&
                  cache->in_weights_buf != nullptr && cache->in_vertpos_buf != nullptr &&
                  cache->in_vertnor_buf != nullptr && cache->in_verttan_buf != nullptr &&
                  (cache->in_bonemat_buf != nullptr || cache->in_bonedq_buf != nullptr));

  cache->buffers_valid = success;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GPU Skinning Shader binding
 *
 * Setup shader buffers for packing and upload
 * \{ */

void draw_skinning_extract_pos_nor_tan(gpu::VertBuf *vbo_pos,
                                       gpu::VertBuf *vbo_nor,
                                       gpu::VertBuf *vbo_tan,
                                       const DRWSkinningCache &cache)
{
  GPU_shader_bind(cache.compute_shader);

  GPU_vertbuf_bind_as_ssbo(cache.in_indices_buf,
                           GPU_shader_get_ssbo_binding(cache.compute_shader, "indices_buf"));

  GPU_vertbuf_bind_as_ssbo(cache.in_weights_buf,
                           GPU_shader_get_ssbo_binding(cache.compute_shader, "weights_buf"));

  if (cache.in_bonedq_buf) {
    GPU_storagebuf_bind(cache.in_bonedq_buf,
                        GPU_shader_get_ssbo_binding(cache.compute_shader,
                                                    "bonedq_buf")); /* Bone Dual Quat buffer */
  }
  else if (cache.in_bonemat_buf) {
    GPU_vertbuf_bind_as_ssbo(cache.in_bonemat_buf,
                             GPU_shader_get_ssbo_binding(cache.compute_shader, "bonemat_buf"));
  }

  GPU_vertbuf_bind_as_ssbo(cache.in_vertpos_buf,
                           GPU_shader_get_ssbo_binding(cache.compute_shader, "pos_buf"));

  GPU_vertbuf_bind_as_ssbo(cache.in_vertnor_buf,
                           GPU_shader_get_ssbo_binding(cache.compute_shader, "nor_buf"));

  GPU_vertbuf_bind_as_ssbo(cache.in_verttan_buf,
                           GPU_shader_get_ssbo_binding(cache.compute_shader, "tan_buf"));

  GPU_vertbuf_bind_as_ssbo(vbo_pos,
                           GPU_shader_get_ssbo_binding(cache.compute_shader, "out_skinned_pos"));

  GPU_vertbuf_bind_as_ssbo(vbo_nor,
                           GPU_shader_get_ssbo_binding(cache.compute_shader, "out_skinned_nor"));

  GPU_vertbuf_bind_as_ssbo(vbo_tan,
                           GPU_shader_get_ssbo_binding(cache.compute_shader, "out_skinned_tan"));

  GPU_shader_uniform_1i(cache.compute_shader, "vertex_count", cache.corner_nums);

  const int workgroups = divide_ceil_u(cache.corner_nums, SKINNING_LOCAL_SIZE);
  GPU_compute_dispatch(cache.compute_shader, workgroups, 1, 1);

  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY);

  GPU_shader_unbind();
}

static gpu::StorageBuf *g_bounds_result_buf = nullptr;
static gpu::StorageBuf *g_original_bounds_buf = nullptr;

void draw_skinning_compute_bounds(Mesh *mesh,
                                  const DRWSkinningCache &cache,
                                  gpu::VertBuf *skinned_positions_vbo)
{
  /* Kinda horrible code for now, but we'll improve later...*/
  auto original_bounds = mesh->bounds_min_max();
  if (!original_bounds) {
    return;
  }

  gpu::Shader *aabb_shader = DRW_shader_armature_skinning_aabb_get();

  if (!g_bounds_result_buf) {
    g_bounds_result_buf = GPU_storagebuf_create(6 * sizeof(uint32_t));
  }

  if (!g_original_bounds_buf) {
    g_original_bounds_buf = GPU_storagebuf_create(2 * sizeof(float4));
  }

  float4 bounds_data[2];
  bounds_data[0] = float4(original_bounds->min, 1.0f);
  bounds_data[1] = float4(original_bounds->max, 1.0f);
  GPU_storagebuf_update(g_original_bounds_buf, bounds_data);

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
  GPU_storagebuf_update(g_bounds_result_buf, init_bounds);

  GPU_shader_bind(aabb_shader);

  GPU_vertbuf_bind_as_ssbo(skinned_positions_vbo, 0);
  GPU_storagebuf_bind(g_original_bounds_buf, 1);
  GPU_storagebuf_bind(g_bounds_result_buf, 2);

  GPU_shader_uniform_1i(aabb_shader, "vertex_count_aabb", cache.corner_nums);

  const int workgroups = divide_ceil_u(cache.corner_nums, 64);
  GPU_compute_dispatch(aabb_shader, workgroups, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE | GPU_BARRIER_VERTEX_ATTRIB_ARRAY);

  GPU_shader_unbind();

  uint32_t result_data[6];
  /* (Ayoub Zouad): this is kinda sad and I hate to read the buffer */
  GPU_storagebuf_read(g_bounds_result_buf, result_data);

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
      [&object_space_bounds](Bounds<float3> &r_data) {
        r_data = object_space_bounds;
      });
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
  if (!draw_skinning_is_available(&ob)) {
    return;
  }

  LISTBASE_FOREACH (ModifierData *, md, &ob.modifiers) {
    if (md->type == eModifierType_Armature) {
      ArmatureModifierData *amd = (ArmatureModifierData *)md;

      DRWSkinningCache &skincache = mesh_batch_cache_ensure_skinning_cache(cache, &ob);

      MeshRenderData mr = mesh_render_data_create(
          ob, mesh, is_editmode, is_paint_mode, do_final, do_uvedit, use_hide, ts);

      if ((U.gpu_flag & USER_GPU_FLAG_SUBDIVISION_EVALUATION) == 0 || (ob.mode & OB_MODE_EDIT)) {
        draw_skinning_cache_free_object(&ob);
      }

      if ((U.gpu_flag & USER_GPU_FLAG_SUBDIVISION_EVALUATION) == 1 && !(ob.mode & OB_MODE_EDIT)) {

        bool flag_changed = (amd && skincache.cached_deform_flag != amd->deformflag);

        bool use_dual_quaternion = (amd->deformflag & ARM_DEF_QUATERNION) != 0;
        bool bone_buffers_exist = use_dual_quaternion ? (skincache.in_bonedq_buf != nullptr) :
                                                        (skincache.in_bonemat_buf != nullptr);

        bool buffers_exist = (skincache.in_indices_buf != nullptr &&
                              skincache.in_weights_buf != nullptr && bone_buffers_exist &&
                              skincache.in_vertpos_buf != nullptr &&
                              skincache.in_vertnor_buf != nullptr &&
                              skincache.compute_shader != nullptr);
        bool needs_buffer_setup = !buffers_exist || flag_changed;

        // TODO (Ayoub Zouad): needs better evaluation for if topo/new modifiers added
        // TODO (Ayoub Zouad): we need to handle multimodifiers
        if (needs_buffer_setup /*|| check mesh/modifier stack updated and or if the mesh's resting data is actively changing*/)
        {
          draw_skinning_cache_free(skincache);
          draw_skinning_setup_buffers(amd->object, &skincache, mr, amd);
          if (!skincache.vertex_data_packed) {

            draw_skinning_pack_vertex_data(amd->object,
                                           &skincache.meshdata_pos,
                                           &skincache.meshdata_nor,
                                           &skincache.meshdata_tan,
                                           &skincache.meshdata_idx,
                                           &skincache.meshdata_wgt,
                                           mr);

            skincache.vertex_data_packed = true;
          }
        }

        if (amd->object && (skincache.bonedata_mat || skincache.bonedata_dq)) {
          bool use_dual_quaternion = (amd->deformflag & ARM_DEF_QUATERNION) != 0;
          draw_skinning_pack_bone_matrices(amd->object,
                                           &ob,
                                           &skincache.bonedata_mat,
                                           &skincache.bonedata_dq,
                                           &skincache.bone_count,
                                           use_dual_quaternion);

          if (skincache.in_bonemat_buf) {
            GPU_vertbuf_tag_dirty(skincache.in_bonemat_buf);
          }
          if (skincache.in_bonedq_buf) {
            GPU_storagebuf_update(skincache.in_bonedq_buf, skincache.bonedata_dq);
          }
        }
        mesh_buffer_cache_create_requested_skinning(
            cache, mbc, ibo_requests, vbo_requests, skincache, mr);
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
