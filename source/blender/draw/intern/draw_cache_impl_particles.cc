/* SPDX-FileCopyrightText: 2017 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Particle API for render engines
 */

#include "DNA_collection_types.h"
#include "DNA_curves_types.h"
#include "DNA_scene_types.h"
#include "DRW_render.hh"

#include "MEM_guardedalloc.h"

#include "BLI_alloca.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_offset_indices.hh"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "DNA_customdata_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_particle_types.h"

#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_legacy_convert.hh"
#include "BKE_particle.h"
#include "BKE_pointcache.h"

#include "ED_particle.hh"

#include "GPU_batch.hh"
#include "GPU_capabilities.hh"
#include "GPU_material.hh"

#include "DEG_depsgraph_query.hh"

#include "draw_cache_impl.hh" /* own include */
#include "draw_hair_private.hh"

namespace blender::draw {

static void particle_batch_cache_clear(ParticleSystem *psys);

/* ---------------------------------------------------------------------- */
/* Particle gpu::Batch Cache */

struct ParticleHairFinalCache {
  /* Output of the subdivision stage: vertex buff sized to subdiv level. */
  blender::gpu::VertBuf *proc_buf;

  /* Just contains a huge index buffer used to draw the final hair. */
  blender::gpu::Batch *proc_hairs[MAX_THICKRES];

  int strands_res; /* points per hair, at least 2 */
};

struct ParticleHairCache {
  blender::gpu::VertBuf *pos;
  blender::gpu::IndexBuf *indices;
  blender::gpu::Batch *hairs;
  int strands_len;
  int elems_len;
  int point_len;

  CurvesEvalCache eval_cache;
};

struct ParticlePointCache {
  gpu::VertBuf *pos;
  gpu::Batch *points;
  int elems_len;
  int point_len;
};

struct ParticleBatchCache {
  /* Object mode strands for hair and points for particle,
   * strands for paths when in edit mode.
   */
  ParticleHairCache hair;   /* Used for hair strands */
  ParticlePointCache point; /* Used for particle points. */

  /* Control points when in edit mode. */
  ParticleHairCache edit_hair;

  gpu::VertBuf *edit_pos;
  gpu::Batch *edit_strands;

  gpu::VertBuf *edit_inner_pos;
  gpu::Batch *edit_inner_points;
  int edit_inner_point_len;

  gpu::VertBuf *edit_tip_pos;
  gpu::Batch *edit_tip_points;
  int edit_tip_point_len;

  /* Settings to determine if cache is invalid. */
  bool is_dirty;
  bool edit_is_weight;
};

/* gpu::Batch cache management. */

struct HairAttributeID {
  uint pos;
  uint tan;
  uint ind;
};

struct EditStrandData {
  float pos[3];
  float selection;
};

static const GPUVertFormat *edit_points_vert_format_get(uint *r_pos_id, uint *r_selection_id)
{
  static uint pos_id, selection_id;
  static const GPUVertFormat edit_point_format = [&]() {
    GPUVertFormat format{};
    pos_id = GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
    selection_id = GPU_vertformat_attr_add(&format, "selection", gpu::VertAttrType::SFLOAT_32);
    return format;
  }();
  *r_pos_id = pos_id;
  *r_selection_id = selection_id;
  return &edit_point_format;
}

static bool particle_batch_cache_valid(ParticleSystem *psys)
{
  ParticleBatchCache *cache = static_cast<ParticleBatchCache *>(psys->batch_cache);

  if (cache == nullptr) {
    return false;
  }

  if (cache->is_dirty == false) {
    return true;
  }

  return false;

  return true;
}

static void particle_batch_cache_init(ParticleSystem *psys)
{
  ParticleBatchCache *cache = static_cast<ParticleBatchCache *>(psys->batch_cache);

  if (!cache) {
    cache = MEM_new<ParticleBatchCache>(__func__);
    psys->batch_cache = cache;
  }
  else {
    cache->edit_hair.eval_cache = {};
    cache->hair.eval_cache = {};
  }

  cache->is_dirty = false;
}

static ParticleBatchCache *particle_batch_cache_get(ParticleSystem *psys)
{
  if (!particle_batch_cache_valid(psys)) {
    particle_batch_cache_clear(psys);
    particle_batch_cache_init(psys);
  }
  return static_cast<ParticleBatchCache *>(psys->batch_cache);
}

void DRW_particle_batch_cache_dirty_tag(ParticleSystem *psys, int mode)
{
  ParticleBatchCache *cache = static_cast<ParticleBatchCache *>(psys->batch_cache);
  if (cache == nullptr) {
    return;
  }
  switch (mode) {
    case BKE_PARTICLE_BATCH_DIRTY_ALL:
      cache->is_dirty = true;
      break;
    default:
      BLI_assert(0);
  }
}

static void particle_batch_cache_clear_point(ParticlePointCache *point_cache)
{
  GPU_BATCH_DISCARD_SAFE(point_cache->points);
  GPU_VERTBUF_DISCARD_SAFE(point_cache->pos);
}

static void particle_batch_cache_clear_hair(ParticleHairCache *hair_cache)
{
  /* TODO: more granular update tagging. */

  /* "Normal" legacy hairs */
  GPU_BATCH_DISCARD_SAFE(hair_cache->hairs);
  GPU_VERTBUF_DISCARD_SAFE(hair_cache->pos);
  GPU_INDEXBUF_DISCARD_SAFE(hair_cache->indices);

  hair_cache->eval_cache.clear();
}

static void particle_batch_cache_clear(ParticleSystem *psys)
{
  ParticleBatchCache *cache = static_cast<ParticleBatchCache *>(psys->batch_cache);
  if (!cache) {
    return;
  }

  /* All memory allocated by `cache` must be freed. */

  particle_batch_cache_clear_point(&cache->point);

  particle_batch_cache_clear_hair(&cache->hair);
  particle_batch_cache_clear_hair(&cache->edit_hair);

  GPU_BATCH_DISCARD_SAFE(cache->edit_inner_points);
  GPU_VERTBUF_DISCARD_SAFE(cache->edit_inner_pos);
  GPU_BATCH_DISCARD_SAFE(cache->edit_tip_points);
  GPU_VERTBUF_DISCARD_SAFE(cache->edit_tip_pos);
}

void DRW_particle_batch_cache_free(ParticleSystem *psys)
{
  particle_batch_cache_clear(psys);
  ParticleBatchCache *batch_cache = static_cast<ParticleBatchCache *>(psys->batch_cache);
  MEM_delete(batch_cache);
  psys->batch_cache = nullptr;
}

void ParticleSpans::foreach_strand(std::function<void(Span<ParticleCacheKey>)> callback)
{
  for (const auto &particle : parent) {
    callback(Span<ParticleCacheKey>(particle, particle->segments + 1));
  }
  for (const auto &particle : children) {
    callback(Span<ParticleCacheKey>(particle, particle->segments + 1));
  }
}

ParticleSpans ParticleDrawSource::particles_get()
{
  if (edit && edit->pathcache) {
    /* Edit particles only display their parent. */
    return {{edit->pathcache, edit->totcached}, {}};
  }

  ParticleSpans spans;
  const bool display_parent = !psys->childcache || (psys->part->draw & PART_DRAW_PARENT);
  if (psys->pathcache && display_parent) {
    spans.parent = {psys->pathcache, psys->totpart};
  }

  if (psys->childcache) {
    spans.children = {psys->childcache, psys->totchild * psys->part->disp / 100};
  }
  return spans;
}

OffsetIndices<int> ParticleDrawSource::points_by_curve()
{
  if (!points_by_curve_storage.is_empty()) {
    return points_by_curve_storage.as_span();
  }

  int total = 0;
  points_by_curve_storage.append(total);
  particles_get().foreach_strand([&](Span<ParticleCacheKey> strand) {
    total += strand.size();
    points_by_curve_storage.append(total);
  });
  return points_by_curve_storage.as_span();
}

static void count_cache_segment_keys(ParticleCacheKey **pathcache,
                                     const int num_path_cache_keys,
                                     ParticleHairCache *hair_cache)
{
  for (int i = 0; i < num_path_cache_keys; i++) {
    ParticleCacheKey *path = pathcache[i];
    if (path->segments > 0) {
      hair_cache->strands_len++;
      hair_cache->elems_len += path->segments + 2;
      hair_cache->point_len += path->segments + 1;
    }
  }
}

static void ensure_seg_pt_count(PTCacheEdit *edit,
                                ParticleSystem *psys,
                                ParticleHairCache *hair_cache)
{
  if (hair_cache->pos != nullptr && hair_cache->indices != nullptr) {
    return;
  }

  hair_cache->strands_len = 0;
  hair_cache->elems_len = 0;
  hair_cache->point_len = 0;

  if (edit != nullptr && edit->pathcache != nullptr) {
    count_cache_segment_keys(edit->pathcache, edit->totcached, hair_cache);
  }
  else {
    if (psys->pathcache && (!psys->childcache || (psys->part->draw & PART_DRAW_PARENT))) {
      count_cache_segment_keys(psys->pathcache, psys->totpart, hair_cache);
    }
    if (psys->childcache) {
      const int child_count = psys->totchild * psys->part->disp / 100;
      count_cache_segment_keys(psys->childcache, child_count, hair_cache);
    }
  }
}

static void particle_pack_mcol(MCol *mcol, ushort r_scol[3])
{
  /* Convert to linear ushort and swizzle */
  r_scol[0] = unit_float_to_ushort_clamp(BLI_color_from_srgb_table[mcol->b]);
  r_scol[1] = unit_float_to_ushort_clamp(BLI_color_from_srgb_table[mcol->g]);
  r_scol[2] = unit_float_to_ushort_clamp(BLI_color_from_srgb_table[mcol->r]);
}

/* Used by parent particles and simple children. */
static void particle_calculate_parent_uvs(ParticleSystem *psys,
                                          ParticleSystemModifierData *psmd,
                                          const int num_uv_layers,
                                          const int parent_index,
                                          const MTFace **mtfaces,
                                          float (*r_uv)[2])
{
  if (psmd == nullptr) {
    return;
  }
  const int emit_from = psmd->psys->part->from;
  if (!ELEM(emit_from, PART_FROM_FACE, PART_FROM_VOLUME)) {
    return;
  }
  ParticleData *particle = &psys->particles[parent_index];
  int num = particle->num_dmcache;
  if (ELEM(num, DMCACHE_NOTFOUND, DMCACHE_ISCHILD)) {
    if (particle->num < psmd->mesh_final->totface_legacy) {
      num = particle->num;
    }
  }
  if (!ELEM(num, DMCACHE_NOTFOUND, DMCACHE_ISCHILD)) {
    const MFace *mfaces = static_cast<const MFace *>(
        CustomData_get_layer(&psmd->mesh_final->fdata_legacy, CD_MFACE));
    if (UNLIKELY(mfaces == nullptr)) {
      BLI_assert_msg(psmd->mesh_final->faces_num == 0,
                     "A mesh with polygons should always have a generated 'CD_MFACE' layer!");
      return;
    }
    const MFace *mface = &mfaces[num];
    for (int j = 0; j < num_uv_layers; j++) {
      psys_interpolate_uvs(mtfaces[j] + num, mface->v4, particle->fuv, r_uv[j]);
    }
  }
}

static void particle_calculate_parent_mcol(ParticleSystem *psys,
                                           ParticleSystemModifierData *psmd,
                                           const int num_col_layers,
                                           const int parent_index,
                                           const MCol **mcols,
                                           MCol *r_mcol)
{
  if (psmd == nullptr) {
    return;
  }
  const int emit_from = psmd->psys->part->from;
  if (!ELEM(emit_from, PART_FROM_FACE, PART_FROM_VOLUME)) {
    return;
  }
  ParticleData *particle = &psys->particles[parent_index];
  int num = particle->num_dmcache;
  if (ELEM(num, DMCACHE_NOTFOUND, DMCACHE_ISCHILD)) {
    if (particle->num < psmd->mesh_final->totface_legacy) {
      num = particle->num;
    }
  }
  if (!ELEM(num, DMCACHE_NOTFOUND, DMCACHE_ISCHILD)) {
    const MFace *mfaces = static_cast<const MFace *>(
        CustomData_get_layer(&psmd->mesh_final->fdata_legacy, CD_MFACE));
    if (UNLIKELY(mfaces == nullptr)) {
      BLI_assert_msg(psmd->mesh_final->faces_num == 0,
                     "A mesh with polygons should always have a generated 'CD_MFACE' layer!");
      return;
    }
    const MFace *mface = &mfaces[num];
    for (int j = 0; j < num_col_layers; j++) {
      /* CustomDataLayer CD_MCOL has 4 structs per face. */
      psys_interpolate_mcol(mcols[j] + num * 4, mface->v4, particle->fuv, &r_mcol[j]);
    }
  }
}

/* Used by interpolated children. */
static void particle_interpolate_children_uvs(ParticleSystem *psys,
                                              ParticleSystemModifierData *psmd,
                                              const int num_uv_layers,
                                              const int child_index,
                                              const MTFace **mtfaces,
                                              float (*r_uv)[2])
{
  if (psmd == nullptr) {
    return;
  }
  const int emit_from = psmd->psys->part->from;
  if (!ELEM(emit_from, PART_FROM_FACE, PART_FROM_VOLUME)) {
    return;
  }
  ChildParticle *particle = &psys->child[child_index];
  int num = particle->num;
  if (num != DMCACHE_NOTFOUND) {
    const MFace *mfaces = static_cast<const MFace *>(
        CustomData_get_layer(&psmd->mesh_final->fdata_legacy, CD_MFACE));
    const MFace *mface = &mfaces[num];
    for (int j = 0; j < num_uv_layers; j++) {
      psys_interpolate_uvs(mtfaces[j] + num, mface->v4, particle->fuv, r_uv[j]);
    }
  }
}

static void particle_interpolate_children_mcol(ParticleSystem *psys,
                                               ParticleSystemModifierData *psmd,
                                               const int num_col_layers,
                                               const int child_index,
                                               const MCol **mcols,
                                               MCol *r_mcol)
{
  if (psmd == nullptr) {
    return;
  }
  const int emit_from = psmd->psys->part->from;
  if (!ELEM(emit_from, PART_FROM_FACE, PART_FROM_VOLUME)) {
    return;
  }
  ChildParticle *particle = &psys->child[child_index];
  int num = particle->num;
  if (num != DMCACHE_NOTFOUND) {
    const MFace *mfaces = static_cast<const MFace *>(
        CustomData_get_layer(&psmd->mesh_final->fdata_legacy, CD_MFACE));
    const MFace *mface = &mfaces[num];
    for (int j = 0; j < num_col_layers; j++) {
      /* CustomDataLayer CD_MCOL has 4 structs per face. */
      psys_interpolate_mcol(mcols[j] + num * 4, mface->v4, particle->fuv, &r_mcol[j]);
    }
  }
}

static void particle_calculate_uvs(ParticleSystem *psys,
                                   ParticleSystemModifierData *psmd,
                                   const bool is_simple,
                                   const int num_uv_layers,
                                   const int parent_index,
                                   const int child_index,
                                   const MTFace **mtfaces,
                                   float (**r_parent_uvs)[2],
                                   float (**r_uv)[2])
{
  if (psmd == nullptr) {
    return;
  }
  if (is_simple) {
    if (r_parent_uvs[parent_index] != nullptr) {
      *r_uv = r_parent_uvs[parent_index];
    }
    else {
      *r_uv = MEM_calloc_arrayN<float[2]>(num_uv_layers, "Particle UVs");
    }
  }
  else {
    *r_uv = MEM_calloc_arrayN<float[2]>(num_uv_layers, "Particle UVs");
  }
  if (child_index == -1) {
    /* Calculate UVs for parent particles. */
    if (is_simple) {
      r_parent_uvs[parent_index] = *r_uv;
    }
    particle_calculate_parent_uvs(psys, psmd, num_uv_layers, parent_index, mtfaces, *r_uv);
  }
  else {
    /* Calculate UVs for child particles. */
    if (!is_simple) {
      particle_interpolate_children_uvs(psys, psmd, num_uv_layers, child_index, mtfaces, *r_uv);
    }
    else if (!r_parent_uvs[psys->child[child_index].parent]) {
      r_parent_uvs[psys->child[child_index].parent] = *r_uv;
      particle_calculate_parent_uvs(psys, psmd, num_uv_layers, parent_index, mtfaces, *r_uv);
    }
  }
}

static void particle_calculate_mcol(ParticleSystem *psys,
                                    ParticleSystemModifierData *psmd,
                                    const bool is_simple,
                                    const int num_col_layers,
                                    const int parent_index,
                                    const int child_index,
                                    const MCol **mcols,
                                    MCol **r_parent_mcol,
                                    MCol **r_mcol)
{
  if (psmd == nullptr) {
    return;
  }
  if (is_simple) {
    if (r_parent_mcol[parent_index] != nullptr) {
      *r_mcol = r_parent_mcol[parent_index];
    }
    else {
      *r_mcol = MEM_calloc_arrayN<MCol>(num_col_layers, "Particle MCol");
    }
  }
  else {
    *r_mcol = MEM_calloc_arrayN<MCol>(num_col_layers, "Particle MCol");
  }
  if (child_index == -1) {
    /* Calculate MCols for parent particles. */
    if (is_simple) {
      r_parent_mcol[parent_index] = *r_mcol;
    }
    particle_calculate_parent_mcol(psys, psmd, num_col_layers, parent_index, mcols, *r_mcol);
  }
  else {
    /* Calculate MCols for child particles. */
    if (!is_simple) {
      particle_interpolate_children_mcol(psys, psmd, num_col_layers, child_index, mcols, *r_mcol);
    }
    else if (!r_parent_mcol[psys->child[child_index].parent]) {
      r_parent_mcol[psys->child[child_index].parent] = *r_mcol;
      particle_calculate_parent_mcol(psys, psmd, num_col_layers, parent_index, mcols, *r_mcol);
    }
  }
}

/* Will return last filled index. */
enum ParticleSource {
  PARTICLE_SOURCE_PARENT,
  PARTICLE_SOURCE_CHILDREN,
};
static int particle_batch_cache_fill_segments(ParticleSystem *psys,
                                              ParticleSystemModifierData *psmd,
                                              ParticleCacheKey **path_cache,
                                              const ParticleSource particle_source,
                                              const int global_offset,
                                              const int start_index,
                                              const int num_path_keys,
                                              const int num_uv_layers,
                                              const int num_col_layers,
                                              const MTFace **mtfaces,
                                              const MCol **mcols,
                                              uint *uv_id,
                                              uint *col_id,
                                              float (***r_parent_uvs)[2],
                                              MCol ***r_parent_mcol,
                                              GPUIndexBufBuilder *elb,
                                              HairAttributeID *attr_id,
                                              ParticleHairCache *hair_cache)
{
  const bool is_simple = (psys->part->childtype == PART_CHILD_PARTICLES);
  const bool is_child = (particle_source == PARTICLE_SOURCE_CHILDREN);
  if (is_simple && *r_parent_uvs == nullptr) {
    /* TODO(sergey): For edit mode it should be edit->totcached. */
    *r_parent_uvs = static_cast<float(**)[2]>(
        MEM_callocN(sizeof(*r_parent_uvs) * psys->totpart, "Parent particle UVs"));
  }
  if (is_simple && *r_parent_mcol == nullptr) {
    *r_parent_mcol = static_cast<MCol **>(
        MEM_callocN(sizeof(*r_parent_mcol) * psys->totpart, "Parent particle MCol"));
  }
  int curr_point = start_index;
  for (int i = 0; i < num_path_keys; i++) {
    ParticleCacheKey *path = path_cache[i];
    if (path->segments <= 0) {
      continue;
    }
    float tangent[3];
    float(*uv)[2] = nullptr;
    MCol *mcol = nullptr;
    particle_calculate_mcol(psys,
                            psmd,
                            is_simple,
                            num_col_layers,
                            is_child ? psys->child[i].parent : i,
                            is_child ? i : -1,
                            mcols,
                            *r_parent_mcol,
                            &mcol);
    particle_calculate_uvs(psys,
                           psmd,
                           is_simple,
                           num_uv_layers,
                           is_child ? psys->child[i].parent : i,
                           is_child ? i : -1,
                           mtfaces,
                           *r_parent_uvs,
                           &uv);
    for (int j = 0; j < path->segments; j++) {
      if (j == 0) {
        sub_v3_v3v3(tangent, path[j + 1].co, path[j].co);
      }
      else {
        sub_v3_v3v3(tangent, path[j + 1].co, path[j - 1].co);
      }
      GPU_vertbuf_attr_set(hair_cache->pos, attr_id->pos, curr_point, path[j].co);
      GPU_vertbuf_attr_set(hair_cache->pos, attr_id->tan, curr_point, tangent);
      GPU_vertbuf_attr_set(hair_cache->pos, attr_id->ind, curr_point, &i);
      if (psmd != nullptr) {
        for (int k = 0; k < num_uv_layers; k++) {
          GPU_vertbuf_attr_set(
              hair_cache->pos,
              uv_id[k],
              curr_point,
              (is_simple && is_child) ? (*r_parent_uvs)[psys->child[i].parent][k] : uv[k]);
        }
        for (int k = 0; k < num_col_layers; k++) {
          /* TODO: Put the conversion outside the loop. */
          ushort scol[4];
          particle_pack_mcol(
              (is_simple && is_child) ? &(*r_parent_mcol)[psys->child[i].parent][k] : &mcol[k],
              scol);
          GPU_vertbuf_attr_set(hair_cache->pos, col_id[k], curr_point, scol);
        }
      }
      GPU_indexbuf_add_generic_vert(elb, curr_point);
      curr_point++;
    }
    sub_v3_v3v3(tangent, path[path->segments].co, path[path->segments - 1].co);

    int global_index = i + global_offset;
    GPU_vertbuf_attr_set(hair_cache->pos, attr_id->pos, curr_point, path[path->segments].co);
    GPU_vertbuf_attr_set(hair_cache->pos, attr_id->tan, curr_point, tangent);
    GPU_vertbuf_attr_set(hair_cache->pos, attr_id->ind, curr_point, &global_index);

    if (psmd != nullptr) {
      for (int k = 0; k < num_uv_layers; k++) {
        GPU_vertbuf_attr_set(hair_cache->pos,
                             uv_id[k],
                             curr_point,
                             (is_simple && is_child) ? (*r_parent_uvs)[psys->child[i].parent][k] :
                                                       uv[k]);
      }
      for (int k = 0; k < num_col_layers; k++) {
        /* TODO: Put the conversion outside the loop. */
        ushort scol[4];
        particle_pack_mcol((is_simple && is_child) ? &(*r_parent_mcol)[psys->child[i].parent][k] :
                                                     &mcol[k],
                           scol);
        GPU_vertbuf_attr_set(hair_cache->pos, col_id[k], curr_point, scol);
      }
      if (!is_simple) {
        MEM_freeN(uv);
        MEM_freeN(mcol);
      }
    }
    /* Finish the segment and add restart primitive. */
    GPU_indexbuf_add_generic_vert(elb, curr_point);
    GPU_indexbuf_add_primitive_restart(elb);
    curr_point++;
  }
  return curr_point;
}

static float particle_key_weight(const ParticleData *particle, int strand, float t)
{
  const ParticleData *part = particle + strand;
  const HairKey *hkeys = part->hair;
  float edit_key_seg_t = 1.0f / (part->totkey - 1);
  if (t == 1.0) {
    return hkeys[part->totkey - 1].weight;
  }

  float interp = t / edit_key_seg_t;
  int index = int(interp);
  interp -= floorf(interp); /* Time between 2 edit key */
  float s1 = hkeys[index].weight;
  float s2 = hkeys[index + 1].weight;
  return s1 + interp * (s2 - s1);
}

static int particle_batch_cache_fill_segments_edit(
    const PTCacheEdit * /*edit*/, /* nullptr for weight data */
    const ParticleData *particle, /* nullptr for select data */
    ParticleCacheKey **path_cache,
    const int start_index,
    const int num_path_keys,
    GPUIndexBufBuilder *elb,
    GPUVertBufRaw *attr_step)
{
  int curr_point = start_index;
  for (int i = 0; i < num_path_keys; i++) {
    ParticleCacheKey *path = path_cache[i];
    if (path->segments <= 0) {
      continue;
    }
    for (int j = 0; j <= path->segments; j++) {
      EditStrandData *seg_data = (EditStrandData *)GPU_vertbuf_raw_step(attr_step);
      copy_v3_v3(seg_data->pos, path[j].co);
      float strand_t = float(j) / path->segments;
      if (particle) {
        float weight = particle_key_weight(particle, i, strand_t);
        /* NaN or unclamped become 1.0f */
        seg_data->selection = (weight < 1.0f) ? weight : 1.0f;
      }
      else {
        /* Computed in psys_cache_edit_paths_iter(). */
        seg_data->selection = path[j].col[0];
      }
      GPU_indexbuf_add_generic_vert(elb, curr_point);
      curr_point++;
    }
    /* Finish the segment and add restart primitive. */
    GPU_indexbuf_add_primitive_restart(elb);
  }
  return curr_point;
}

static int particle_batch_cache_fill_strands_data(ParticleSystem *psys,
                                                  ParticleSystemModifierData *psmd,
                                                  ParticleCacheKey **path_cache,
                                                  const ParticleSource particle_source,
                                                  const int start_index,
                                                  const int num_path_keys,
                                                  GPUVertBufRaw *data_step,
                                                  GPUVertBufRaw *seg_step,
                                                  float (***r_parent_uvs)[2],
                                                  GPUVertBufRaw *uv_step,
                                                  const MTFace **mtfaces,
                                                  int num_uv_layers,
                                                  MCol ***r_parent_mcol,
                                                  GPUVertBufRaw *col_step,
                                                  const MCol **mcols,
                                                  int num_col_layers)
{
  const bool is_simple = (psys->part->childtype == PART_CHILD_PARTICLES);
  const bool is_child = (particle_source == PARTICLE_SOURCE_CHILDREN);
  if (is_simple && *r_parent_uvs == nullptr) {
    /* TODO(sergey): For edit mode it should be edit->totcached. */
    *r_parent_uvs = static_cast<float(**)[2]>(
        MEM_callocN(sizeof(*r_parent_uvs) * psys->totpart, "Parent particle UVs"));
  }
  if (is_simple && *r_parent_mcol == nullptr) {
    *r_parent_mcol = static_cast<MCol **>(
        MEM_callocN(sizeof(*r_parent_mcol) * psys->totpart, "Parent particle MCol"));
  }
  int curr_point = start_index;
  for (int i = 0; i < num_path_keys; i++) {
    ParticleCacheKey *path = path_cache[i];
    if (path->segments <= 0) {
      continue;
    }

    *(uint *)GPU_vertbuf_raw_step(data_step) = curr_point;
    *(uint *)GPU_vertbuf_raw_step(seg_step) = path->segments;
    curr_point += path->segments + 1;

    if (psmd != nullptr) {
      float(*uv)[2] = nullptr;
      MCol *mcol = nullptr;

      particle_calculate_uvs(psys,
                             psmd,
                             is_simple,
                             num_uv_layers,
                             is_child ? psys->child[i].parent : i,
                             is_child ? i : -1,
                             mtfaces,
                             *r_parent_uvs,
                             &uv);

      particle_calculate_mcol(psys,
                              psmd,
                              is_simple,
                              num_col_layers,
                              is_child ? psys->child[i].parent : i,
                              is_child ? i : -1,
                              mcols,
                              *r_parent_mcol,
                              &mcol);

      for (int k = 0; k < num_uv_layers; k++) {
        float *t_uv = (float *)GPU_vertbuf_raw_step(uv_step + k);
        copy_v2_v2(t_uv, uv[k]);
      }
      for (int k = 0; k < num_col_layers; k++) {
        ushort *scol = (ushort *)GPU_vertbuf_raw_step(col_step + k);
        particle_pack_mcol((is_simple && is_child) ? &(*r_parent_mcol)[psys->child[i].parent][k] :
                                                     &mcol[k],
                           scol);
      }
      if (!is_simple) {
        MEM_freeN(uv);
        MEM_freeN(mcol);
      }
    }
  }
  return curr_point;
}

static void particle_batch_cache_ensure_pos_and_seg(PTCacheEdit *edit,
                                                    ParticleSystem *psys,
                                                    ModifierData *md,
                                                    ParticleHairCache *hair_cache)
{
  if (hair_cache->pos != nullptr && hair_cache->indices != nullptr) {
    return;
  }

  int curr_point = 0;
  ParticleSystemModifierData *psmd = (ParticleSystemModifierData *)md;

  GPU_VERTBUF_DISCARD_SAFE(hair_cache->pos);
  GPU_INDEXBUF_DISCARD_SAFE(hair_cache->indices);

  GPUVertFormat format = {0};
  HairAttributeID attr_id;
  uint *uv_id = nullptr;
  uint *col_id = nullptr;
  int num_uv_layers = 0;
  int num_col_layers = 0;
  int active_uv = 0;
  int active_col = 0;
  const MTFace **mtfaces = nullptr;
  const MCol **mcols = nullptr;
  float(**parent_uvs)[2] = nullptr;
  MCol **parent_mcol = nullptr;

  if (psmd != nullptr) {
    if (CustomData_has_layer(&psmd->mesh_final->corner_data, CD_PROP_FLOAT2)) {
      num_uv_layers = CustomData_number_of_layers(&psmd->mesh_final->corner_data, CD_PROP_FLOAT2);
      active_uv = CustomData_get_active_layer(&psmd->mesh_final->corner_data, CD_PROP_FLOAT2);
    }
    if (CustomData_has_layer(&psmd->mesh_final->corner_data, CD_PROP_BYTE_COLOR)) {
      num_col_layers = CustomData_number_of_layers(&psmd->mesh_final->corner_data,
                                                   CD_PROP_BYTE_COLOR);
      if (psmd->mesh_final->active_color_attribute != nullptr) {
        active_col = CustomData_get_named_layer(&psmd->mesh_final->corner_data,
                                                CD_PROP_BYTE_COLOR,
                                                psmd->mesh_final->active_color_attribute);
      }
    }
  }

  attr_id.pos = GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  attr_id.tan = GPU_vertformat_attr_add(&format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);
  attr_id.ind = GPU_vertformat_attr_add(&format, "ind", gpu::VertAttrType::SINT_32);

  if (psmd) {
    uv_id = MEM_malloc_arrayN<uint>(num_uv_layers, "UV attr format");
    col_id = MEM_malloc_arrayN<uint>(num_col_layers, "Col attr format");

    for (int i = 0; i < num_uv_layers; i++) {

      char uuid[32], attr_safe_name[GPU_MAX_SAFE_ATTR_NAME];
      const char *name = CustomData_get_layer_name(
          &psmd->mesh_final->corner_data, CD_PROP_FLOAT2, i);
      GPU_vertformat_safe_attr_name(name, attr_safe_name, GPU_MAX_SAFE_ATTR_NAME);

      SNPRINTF_UTF8(uuid, "a%s", attr_safe_name);
      uv_id[i] = GPU_vertformat_attr_add(&format, uuid, blender::gpu::VertAttrType::SFLOAT_32_32);

      if (i == active_uv) {
        GPU_vertformat_alias_add(&format, "a");
      }
    }

    for (int i = 0; i < num_col_layers; i++) {
      char uuid[32], attr_safe_name[GPU_MAX_SAFE_ATTR_NAME];
      const char *name = CustomData_get_layer_name(
          &psmd->mesh_final->corner_data, CD_PROP_BYTE_COLOR, i);
      GPU_vertformat_safe_attr_name(name, attr_safe_name, GPU_MAX_SAFE_ATTR_NAME);

      SNPRINTF_UTF8(uuid, "a%s", attr_safe_name);
      col_id[i] = GPU_vertformat_attr_add(
          &format, uuid, blender::gpu::VertAttrType::UNORM_16_16_16_16);

      if (i == active_col) {
        GPU_vertformat_alias_add(&format, "c");
      }
    }
  }

  hair_cache->pos = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*hair_cache->pos, hair_cache->point_len);

  GPUIndexBufBuilder elb;
  GPU_indexbuf_init_ex(&elb, GPU_PRIM_LINE_STRIP, hair_cache->elems_len, hair_cache->point_len);

  if (num_uv_layers || num_col_layers) {
    BKE_mesh_tessface_ensure(psmd->mesh_final);
    if (num_uv_layers) {
      mtfaces = static_cast<const MTFace **>(
          MEM_mallocN(sizeof(*mtfaces) * num_uv_layers, "Faces UV layers"));
      for (int i = 0; i < num_uv_layers; i++) {
        mtfaces[i] = (const MTFace *)CustomData_get_layer_n(
            &psmd->mesh_final->fdata_legacy, CD_MTFACE, i);
      }
    }
    if (num_col_layers) {
      mcols = static_cast<const MCol **>(
          MEM_mallocN(sizeof(*mcols) * num_col_layers, "Color layers"));
      for (int i = 0; i < num_col_layers; i++) {
        mcols[i] = (const MCol *)CustomData_get_layer_n(
            &psmd->mesh_final->fdata_legacy, CD_MCOL, i);
      }
    }
  }

  if (edit != nullptr && edit->pathcache != nullptr) {
    curr_point = particle_batch_cache_fill_segments(psys,
                                                    psmd,
                                                    edit->pathcache,
                                                    PARTICLE_SOURCE_PARENT,
                                                    0,
                                                    0,
                                                    edit->totcached,
                                                    num_uv_layers,
                                                    num_col_layers,
                                                    mtfaces,
                                                    mcols,
                                                    uv_id,
                                                    col_id,
                                                    &parent_uvs,
                                                    &parent_mcol,
                                                    &elb,
                                                    &attr_id,
                                                    hair_cache);
  }
  else {
    if ((psys->pathcache != nullptr) &&
        (!psys->childcache || (psys->part->draw & PART_DRAW_PARENT)))
    {
      curr_point = particle_batch_cache_fill_segments(psys,
                                                      psmd,
                                                      psys->pathcache,
                                                      PARTICLE_SOURCE_PARENT,
                                                      0,
                                                      0,
                                                      psys->totpart,
                                                      num_uv_layers,
                                                      num_col_layers,
                                                      mtfaces,
                                                      mcols,
                                                      uv_id,
                                                      col_id,
                                                      &parent_uvs,
                                                      &parent_mcol,
                                                      &elb,
                                                      &attr_id,
                                                      hair_cache);
    }
    if (psys->childcache != nullptr) {
      const int child_count = psys->totchild * psys->part->disp / 100;
      curr_point = particle_batch_cache_fill_segments(psys,
                                                      psmd,
                                                      psys->childcache,
                                                      PARTICLE_SOURCE_CHILDREN,
                                                      psys->totpart,
                                                      curr_point,
                                                      child_count,
                                                      num_uv_layers,
                                                      num_col_layers,
                                                      mtfaces,
                                                      mcols,
                                                      uv_id,
                                                      col_id,
                                                      &parent_uvs,
                                                      &parent_mcol,
                                                      &elb,
                                                      &attr_id,
                                                      hair_cache);
    }
  }
  /* Cleanup. */
  if (parent_uvs != nullptr) {
    /* TODO(sergey): For edit mode it should be edit->totcached. */
    for (int i = 0; i < psys->totpart; i++) {
      MEM_SAFE_FREE(parent_uvs[i]);
    }
    MEM_freeN(parent_uvs);
  }
  if (parent_mcol != nullptr) {
    for (int i = 0; i < psys->totpart; i++) {
      MEM_SAFE_FREE(parent_mcol[i]);
    }
    MEM_freeN(parent_mcol);
  }
  if (num_uv_layers) {
    MEM_freeN(mtfaces);
  }
  if (num_col_layers) {
    MEM_freeN(mcols);
  }
  if (psmd != nullptr) {
    MEM_freeN(uv_id);
  }
  hair_cache->indices = GPU_indexbuf_build(&elb);
}

static void particle_batch_cache_ensure_pos(Object *object,
                                            ParticleSystem *psys,
                                            ParticlePointCache *point_cache)
{
  if (point_cache->pos != nullptr) {
    return;
  }

  int i, curr_point;
  ParticleData *pa;
  ParticleKey state;
  ParticleSimulationData sim = {nullptr};
  const DRWContext *draw_ctx = DRW_context_get();

  sim.depsgraph = draw_ctx->depsgraph;
  sim.scene = draw_ctx->scene;
  sim.ob = object;
  sim.psys = psys;
  sim.psmd = psys_get_modifier(object, psys);
  psys_sim_data_init(&sim);

  GPU_VERTBUF_DISCARD_SAFE(point_cache->pos);

  static uint pos_id, rot_id, val_id;
  static const GPUVertFormat format = [&]() {
    GPUVertFormat format{};
    pos_id = GPU_vertformat_attr_add(&format, "part_pos", gpu::VertAttrType::SFLOAT_32_32_32);
    val_id = GPU_vertformat_attr_add(&format, "part_val", gpu::VertAttrType::SFLOAT_32);
    rot_id = GPU_vertformat_attr_add(&format, "part_rot", gpu::VertAttrType::SFLOAT_32_32_32_32);
    return format;
  }();

  point_cache->pos = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*point_cache->pos, psys->totpart);

  for (curr_point = 0, i = 0, pa = psys->particles; i < psys->totpart; i++, pa++) {
    state.time = DEG_get_ctime(draw_ctx->depsgraph);
    if (!psys_get_particle_state(&sim, i, &state, false)) {
      continue;
    }

    float val;

    GPU_vertbuf_attr_set(point_cache->pos, pos_id, curr_point, state.co);
    GPU_vertbuf_attr_set(point_cache->pos, rot_id, curr_point, state.rot);

    switch (psys->part->draw_col) {
      case PART_DRAW_COL_VEL:
        val = len_v3(state.vel) / psys->part->color_vec_max;
        break;
      case PART_DRAW_COL_ACC:
        val = len_v3v3(state.vel, pa->prev_state.vel) /
              ((state.time - pa->prev_state.time) * psys->part->color_vec_max);
        break;
      default:
        val = -1.0f;
        break;
    }

    GPU_vertbuf_attr_set(point_cache->pos, val_id, curr_point, &val);

    curr_point++;
  }

  if (curr_point != psys->totpart) {
    GPU_vertbuf_data_resize(*point_cache->pos, curr_point);
  }

  psys_sim_data_free(&sim);
}

static void drw_particle_update_ptcache_edit(Object *object_eval,
                                             ParticleSystem *psys,
                                             PTCacheEdit *edit)
{
  if (edit->psys == nullptr) {
    return;
  }
  /* NOTE: Get flag from particle system coming from drawing object.
   * this is where depsgraph will be setting flags to.
   */
  const DRWContext *draw_ctx = DRW_context_get();
  Scene *scene_orig = DEG_get_original(draw_ctx->scene);
  Object *object_orig = DEG_get_original(object_eval);
  if (psys->flag & PSYS_HAIR_UPDATED) {
    PE_update_object(draw_ctx->depsgraph, scene_orig, object_orig, 0);
    psys->flag &= ~PSYS_HAIR_UPDATED;
  }
  if (edit->pathcache == nullptr) {
    Depsgraph *depsgraph = draw_ctx->depsgraph;
    psys_cache_edit_paths(depsgraph,
                          scene_orig,
                          object_orig,
                          edit,
                          DEG_get_ctime(depsgraph),
                          DEG_get_mode(depsgraph) == DAG_EVAL_RENDER);
  }
}

void drw_particle_update_ptcache(Object *object_eval, ParticleSystem *psys)
{
  if ((object_eval->mode & OB_MODE_PARTICLE_EDIT) == 0) {
    return;
  }
  const DRWContext *draw_ctx = DRW_context_get();
  Scene *scene_orig = DEG_get_original(draw_ctx->scene);
  Object *object_orig = DEG_get_original(object_eval);
  PTCacheEdit *edit = PE_create_current(draw_ctx->depsgraph, scene_orig, object_orig);
  if (edit != nullptr) {
    drw_particle_update_ptcache_edit(object_eval, psys, edit);
  }
}

ParticleDrawSource drw_particle_get_hair_source(Object *object,
                                                ParticleSystem *psys,
                                                ModifierData *md,
                                                PTCacheEdit *edit)
{
  const DRWContext *draw_ctx = DRW_context_get();
  ParticleDrawSource src;
  src.object = object;
  src.psys = psys;
  src.md = md;
  src.edit = edit;
  if (psys_in_edit_mode(draw_ctx->depsgraph, psys)) {
    src.object = DEG_get_original(object);
    src.psys = psys_orig_get(psys);
  }
  return src;
}

gpu::Batch *DRW_particles_batch_cache_get_hair(Object *object,
                                               ParticleSystem *psys,
                                               ModifierData *md)
{
  ParticleBatchCache *cache = particle_batch_cache_get(psys);
  if (cache->hair.hairs == nullptr) {
    drw_particle_update_ptcache(object, psys);
    ParticleDrawSource source = drw_particle_get_hair_source(object, psys, md, nullptr);
    ensure_seg_pt_count(source.edit, source.psys, &cache->hair);
    particle_batch_cache_ensure_pos_and_seg(source.edit, source.psys, source.md, &cache->hair);
    cache->hair.hairs = GPU_batch_create(
        GPU_PRIM_LINE_STRIP, cache->hair.pos, cache->hair.indices);
  }
  return cache->hair.hairs;
}

gpu::Batch *DRW_particles_batch_cache_get_dots(Object *object, ParticleSystem *psys)
{
  ParticleBatchCache *cache = particle_batch_cache_get(psys);

  if (cache->point.points == nullptr) {
    particle_batch_cache_ensure_pos(object, psys, &cache->point);
    cache->point.points = GPU_batch_create(GPU_PRIM_POINTS, cache->point.pos, nullptr);
  }

  return cache->point.points;
}

static void particle_batch_cache_ensure_edit_pos_and_seg(PTCacheEdit *edit,
                                                         ParticleSystem *psys,
                                                         ModifierData * /*md*/,
                                                         ParticleHairCache *hair_cache,
                                                         bool use_weight)
{
  if (hair_cache->pos != nullptr && hair_cache->indices != nullptr) {
    return;
  }

  ParticleData *particle = (use_weight) ? psys->particles : nullptr;

  GPU_VERTBUF_DISCARD_SAFE(hair_cache->pos);
  GPU_INDEXBUF_DISCARD_SAFE(hair_cache->indices);

  GPUVertBufRaw data_step;
  GPUIndexBufBuilder elb;
  uint pos_id, selection_id;
  const GPUVertFormat *edit_point_format = edit_points_vert_format_get(&pos_id, &selection_id);

  hair_cache->pos = GPU_vertbuf_create_with_format(*edit_point_format);
  GPU_vertbuf_data_alloc(*hair_cache->pos, hair_cache->point_len);
  GPU_vertbuf_attr_get_raw_data(hair_cache->pos, pos_id, &data_step);

  GPU_indexbuf_init_ex(&elb, GPU_PRIM_LINE_STRIP, hair_cache->elems_len, hair_cache->point_len);

  if (edit != nullptr && edit->pathcache != nullptr) {
    particle_batch_cache_fill_segments_edit(
        edit, particle, edit->pathcache, 0, edit->totcached, &elb, &data_step);
  }
  hair_cache->indices = GPU_indexbuf_build(&elb);
}

gpu::Batch *DRW_particles_batch_cache_get_edit_strands(Object *object,
                                                       ParticleSystem *psys,
                                                       PTCacheEdit *edit,
                                                       bool use_weight)
{
  ParticleBatchCache *cache = particle_batch_cache_get(psys);
  if (cache->edit_is_weight != use_weight) {
    GPU_VERTBUF_DISCARD_SAFE(cache->edit_hair.pos);
    GPU_BATCH_DISCARD_SAFE(cache->edit_hair.hairs);
  }
  if (cache->edit_hair.hairs != nullptr) {
    return cache->edit_hair.hairs;
  }
  drw_particle_update_ptcache_edit(object, psys, edit);
  ensure_seg_pt_count(edit, psys, &cache->edit_hair);
  particle_batch_cache_ensure_edit_pos_and_seg(edit, psys, nullptr, &cache->edit_hair, use_weight);
  cache->edit_hair.hairs = GPU_batch_create(
      GPU_PRIM_LINE_STRIP, cache->edit_hair.pos, cache->edit_hair.indices);
  cache->edit_is_weight = use_weight;
  return cache->edit_hair.hairs;
}

static void ensure_edit_inner_points_count(const PTCacheEdit *edit, ParticleBatchCache *cache)
{
  if (cache->edit_inner_pos != nullptr) {
    return;
  }
  cache->edit_inner_point_len = 0;
  for (int point_index = 0; point_index < edit->totpoint; point_index++) {
    const PTCacheEditPoint *point = &edit->points[point_index];
    if (point->flag & PEP_HIDE) {
      continue;
    }
    BLI_assert(point->totkey >= 1);
    cache->edit_inner_point_len += (point->totkey - 1);
  }
}

static void particle_batch_cache_ensure_edit_inner_pos(PTCacheEdit *edit,
                                                       ParticleBatchCache *cache)
{
  if (cache->edit_inner_pos != nullptr) {
    return;
  }

  uint pos_id, selection_id;
  const GPUVertFormat *edit_point_format = edit_points_vert_format_get(&pos_id, &selection_id);

  cache->edit_inner_pos = GPU_vertbuf_create_with_format(*edit_point_format);
  GPU_vertbuf_data_alloc(*cache->edit_inner_pos, cache->edit_inner_point_len);

  int global_key_index = 0;
  for (int point_index = 0; point_index < edit->totpoint; point_index++) {
    const PTCacheEditPoint *point = &edit->points[point_index];
    if (point->flag & PEP_HIDE) {
      continue;
    }
    for (int key_index = 0; key_index < point->totkey - 1; key_index++) {
      PTCacheEditKey *key = &point->keys[key_index];
      float selection = (key->flag & PEK_SELECT) ? 1.0f : 0.0f;
      GPU_vertbuf_attr_set(cache->edit_inner_pos, pos_id, global_key_index, key->world_co);
      GPU_vertbuf_attr_set(cache->edit_inner_pos, selection_id, global_key_index, &selection);
      global_key_index++;
    }
  }
}

gpu::Batch *DRW_particles_batch_cache_get_edit_inner_points(Object *object,
                                                            ParticleSystem *psys,
                                                            PTCacheEdit *edit)
{
  ParticleBatchCache *cache = particle_batch_cache_get(psys);
  if (cache->edit_inner_points != nullptr) {
    return cache->edit_inner_points;
  }
  drw_particle_update_ptcache_edit(object, psys, edit);
  ensure_edit_inner_points_count(edit, cache);
  particle_batch_cache_ensure_edit_inner_pos(edit, cache);
  cache->edit_inner_points = GPU_batch_create(GPU_PRIM_POINTS, cache->edit_inner_pos, nullptr);
  return cache->edit_inner_points;
}

static void ensure_edit_tip_points_count(const PTCacheEdit *edit, ParticleBatchCache *cache)
{
  if (cache->edit_tip_pos != nullptr) {
    return;
  }
  cache->edit_tip_point_len = 0;
  for (int point_index = 0; point_index < edit->totpoint; point_index++) {
    const PTCacheEditPoint *point = &edit->points[point_index];
    if (point->flag & PEP_HIDE) {
      continue;
    }
    cache->edit_tip_point_len += 1;
  }
}

static void particle_batch_cache_ensure_edit_tip_pos(PTCacheEdit *edit, ParticleBatchCache *cache)
{
  if (cache->edit_tip_pos != nullptr) {
    return;
  }

  uint pos_id, selection_id;
  const GPUVertFormat *edit_point_format = edit_points_vert_format_get(&pos_id, &selection_id);

  cache->edit_tip_pos = GPU_vertbuf_create_with_format(*edit_point_format);
  GPU_vertbuf_data_alloc(*cache->edit_tip_pos, cache->edit_tip_point_len);

  int global_point_index = 0;
  for (int point_index = 0; point_index < edit->totpoint; point_index++) {
    const PTCacheEditPoint *point = &edit->points[point_index];
    if (point->flag & PEP_HIDE) {
      continue;
    }
    PTCacheEditKey *key = &point->keys[point->totkey - 1];
    float selection = (key->flag & PEK_SELECT) ? 1.0f : 0.0f;

    GPU_vertbuf_attr_set(cache->edit_tip_pos, pos_id, global_point_index, key->world_co);
    GPU_vertbuf_attr_set(cache->edit_tip_pos, selection_id, global_point_index, &selection);
    global_point_index++;
  }
}

gpu::Batch *DRW_particles_batch_cache_get_edit_tip_points(Object *object,
                                                          ParticleSystem *psys,
                                                          PTCacheEdit *edit)
{
  ParticleBatchCache *cache = particle_batch_cache_get(psys);
  if (cache->edit_tip_points != nullptr) {
    return cache->edit_tip_points;
  }
  drw_particle_update_ptcache_edit(object, psys, edit);
  ensure_edit_tip_points_count(edit, cache);
  particle_batch_cache_ensure_edit_tip_pos(edit, cache);
  cache->edit_tip_points = GPU_batch_create(GPU_PRIM_POINTS, cache->edit_tip_pos, nullptr);
  return cache->edit_tip_points;
}

void CurvesEvalCache::ensure_attributes(CurvesModule &module,
                                        ParticleDrawSource &src,
                                        const GPUMaterial *gpu_material,
                                        int additional_subdivision)
{
  /* TODO. */
}

void CurvesEvalCache::ensure_common(ParticleDrawSource &src)
{
  if (points_by_curve_buf) {
    return;
  }

  points_by_curve_buf = gpu::VertBuf::new_from_span(src.points_by_curve().data());
  /* TODO subdiv. */
  evaluated_points_by_curve_buf = gpu::VertBuf::new_from_span(src.points_by_curve().data());

  /* Use the same type for all curves. */
  /* TODO subdiv. */
  auto type_varray = VArray<int8_t>::from_single(CURVE_TYPE_CATMULL_ROM, src.curves_num());
  auto resolution_varray = VArray<int32_t>::from_single(1, src.curves_num());
  /* Not used. */
  auto cyclic_offsets_varray = VArray<int32_t>::from_single(0, 2);
  /* TODO(fclem): Optimize shaders to avoid needing to upload this data if data is uniform.
   * This concerns all varray. */
  curves_type_buf = gpu::VertBuf::new_from_varray(type_varray);
  curves_resolution_buf = gpu::VertBuf::new_from_varray(resolution_varray);
  curves_cyclic_buf = gpu::VertBuf::new_from_varray(cyclic_offsets_varray);
}

/* Copied from cycles. */
static float hair_shape_radius(float shape, float root, float tip, float time)
{
  float radius = 1.0f - time;
  if (shape < 0.0f) {
    radius = pow(radius, 1.0f + shape);
  }
  else {
    radius = pow(radius, 1.0f / (1.0f - shape));
  }
  return (radius * (root - tip)) + tip;
}

void CurvesEvalCache::ensure_positions(CurvesModule &module,
                                       ParticleDrawSource &src,
                                       int additional_subdivision)
{
  if (evaluated_pos_rad_buf) {
    return;
  }

  ensure_common(src);

  gpu::VertBufPtr points_pos_buf = gpu::VertBuf::new_from_size<float3>(src.points_num());
  gpu::VertBufPtr points_rad_buf = gpu::VertBuf::new_from_size<float>(src.points_num());

  MutableSpan<float3> points_pos = points_pos_buf->data<float3>();
  MutableSpan<float> points_rad = points_rad_buf->data<float>();

  const ParticleSettings &part = *src.psys->part;
  const float hair_rad_shape = part.shape;
  const float hair_rad_root = part.rad_root * part.rad_scale * 0.5f;
  const float hair_rad_tip = part.rad_tip * part.rad_scale * 0.5f;
  const bool hair_close_tip = (part.shape_flag & PART_SHAPE_CLOSE_TIP) != 0;

  int i = 0;
  src.particles_get().foreach_strand([&](Span<ParticleCacheKey> strand) {
    int j = 0;
    for (const ParticleCacheKey &point : strand) {
      points_pos[i] = point.co;
      points_rad[i] = (hair_close_tip && (j == strand.index_range().last())) ?
                          0.0f :
                          hair_shape_radius(
                              hair_rad_shape, hair_rad_root, hair_rad_tip, point.time);
      i++, j++;
    }
  });

  evaluated_pos_rad_buf = gpu::VertBuf::new_device_only<float4>(src.evaluated_points_num());

  float4x4 transform = src.object->world_to_object();

  module.evaluate_positions(true,
                            false,
                            false,
                            false,
                            false,
                            src.curves_num(),
                            *this,
                            std::move(points_pos_buf),
                            std::move(points_rad_buf),
                            evaluated_pos_rad_buf,
                            transform);

  /* TODO(fclem): Make time and length optional. */
  evaluated_time_buf = gpu::VertBuf::new_device_only<float>(src.evaluated_points_num());
  curves_length_buf = gpu::VertBuf::new_device_only<float>(src.curves_num());

  module.evaluate_curve_length_intercept(false, src.curves_num(), *this);
}

gpu::VertBufPtr &CurvesEvalCache::indirection_buf_get(CurvesModule &module,
                                                      ParticleDrawSource &src,
                                                      int face_per_segment)
{
  const bool is_ribbon = face_per_segment < 2;

  gpu::VertBufPtr &indirection_buf = is_ribbon ? this->indirection_ribbon_buf :
                                                 this->indirection_cylinder_buf;
  if (indirection_buf) {
    return indirection_buf;
  }

  ensure_common(src);

  indirection_buf = module.evaluate_topology_indirection(
      src.curves_num(), src.evaluated_points_num(), *this, is_ribbon, false);

  return indirection_buf;
}

CurvesEvalCache &hair_particle_get_eval_cache(ParticleDrawSource &src)
{
  ParticleBatchCache *cache = particle_batch_cache_get(src.psys);
  return cache->hair.eval_cache;
}

}  // namespace blender::draw
