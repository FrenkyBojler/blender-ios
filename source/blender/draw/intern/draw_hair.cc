/* SPDX-FileCopyrightText: 2017 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Contains procedural GPU hair drawing methods.
 */

#include "BKE_customdata.hh"

#include "DNA_modifier_types.h"
#include "DNA_particle_types.h"
#include "DNA_scene_types.h"

#include "DRW_render.hh"

#include "GPU_batch.hh"
#include "GPU_material.hh"
#include "GPU_shader.hh"
#include "GPU_texture.hh"
#include "GPU_vertex_buffer.hh"

#include "draw_common_c.hh"
#include "draw_context_private.hh"
#include "draw_hair_private.hh"
#include "draw_manager.hh"
#include "draw_shader_shared.hh"

/* New Draw Manager. */
#include "draw_common.hh"

namespace blender::draw {

blender::gpu::VertBuf *hair_pos_buffer_get(Scene *scene,
                                           Object *object,
                                           ParticleSystem *psys,
                                           ModifierData *md)
{
  /* TODO(fclem): Remove Global access. */
  CurvesModule &module = *drw_get().data->curves_module;

  drw_particle_update_ptcache(object, psys);
  ParticleDrawSource source = drw_particle_get_hair_source(
      object, psys, md, nullptr, scene->r.hair_subdiv);

  CurvesEvalCache &cache = hair_particle_get_eval_cache(source);
  cache.ensure_positions(module, source);

  return cache.evaluated_pos_rad_buf.get();
}

template<typename PassT>
blender::gpu::Batch *hair_sub_pass_setup_implementation(PassT &sub_ps,
                                                        const Scene *scene,
                                                        const ObjectRef &ob_ref,
                                                        ParticleSystem *psys,
                                                        ModifierData *md,
                                                        GPUMaterial *gpu_material)
{
  /** NOTE: This still relies on the old DRW_hair implementation. */
  Object *object = ob_ref.object;

  drw_particle_update_ptcache(object, psys);

  /* TODO(fclem): Remove Global access. */
  CurvesModule &module = *drw_get().data->curves_module;

  /* Ensure we have no unbound resources.
   * Required for Vulkan.
   * Fixes issues with certain GL drivers not drawing anything. */
  sub_ps.bind_texture("u", module.dummy_vbo);
  sub_ps.bind_texture("au", module.dummy_vbo);
  sub_ps.bind_texture("a", module.dummy_vbo);
  sub_ps.bind_texture("c", module.dummy_vbo);
  sub_ps.bind_texture("ac", module.dummy_vbo);
  if (gpu_material) {
    ListBase attr_list = GPU_material_attributes(gpu_material);
    ListBaseWrapper<GPUMaterialAttribute> attrs(attr_list);
    for (const GPUMaterialAttribute *attr : attrs) {
      sub_ps.bind_texture(attr->input_name, module.dummy_vbo);
    }
  }

  /* TODO: optimize this. Only bind the ones #GPUMaterial needs. */
  for (int i : IndexRange(hair_cache->num_uv_layers)) {
    for (int n = 0; n < MAX_LAYER_NAME_CT && hair_cache->uv_layer_names[i][n][0] != '\0'; n++) {
      sub_ps.bind_texture(hair_cache->uv_layer_names[i][n], hair_cache->uv_tex[i]);
    }
  }
  for (int i : IndexRange(hair_cache->num_col_layers)) {
    for (int n = 0; n < MAX_LAYER_NAME_CT && hair_cache->col_layer_names[i][n][0] != '\0'; n++) {
      sub_ps.bind_texture(hair_cache->col_layer_names[i][n], hair_cache->col_tex[i]);
    }
  }

  float4x4 dupli_mat;
  DRW_hair_duplimat_get(ob_ref, psys, md, dupli_mat.ptr());

  /* Get hair shape parameters. */
  ParticleSettings *part = psys->part;
  float hair_rad_shape = part->shape;
  float hair_rad_root = part->rad_root * part->rad_scale * 0.5f;
  float hair_rad_tip = part->rad_tip * part->rad_scale * 0.5f;
  bool hair_close_tip = (part->shape_flag & PART_SHAPE_CLOSE_TIP) != 0;

  sub_ps.bind_texture("hairPointBuffer", hair_cache->final[subdiv].proc_buf);
  if (hair_cache->proc_length_buf) {
    sub_ps.bind_texture("l", hair_cache->proc_length_buf);
  }

  sub_ps.bind_ubo("drw_curves", module.ubo_pool.dummy_get());
  sub_ps.push_constant("hairStrandsRes", &hair_cache->final[subdiv].strands_res, 1);
  sub_ps.push_constant("hairThicknessRes", thickness_res);
  sub_ps.push_constant("hairRadShape", hair_rad_shape);
  sub_ps.push_constant("hairDupliMatrix", dupli_mat);
  sub_ps.push_constant("hairRadRoot", hair_rad_root);
  sub_ps.push_constant("hairRadTip", hair_rad_tip);
  sub_ps.push_constant("hairCloseTip", hair_close_tip);

  return hair_cache->final[subdiv].proc_hairs[thickness_res - 1];
}

blender::gpu::Batch *hair_sub_pass_setup(PassMain::Sub &sub_ps,
                                         const Scene *scene,
                                         const ObjectRef &ob_ref,
                                         ParticleSystem *psys,
                                         ModifierData *md,
                                         GPUMaterial *gpu_material)
{
  return hair_sub_pass_setup_implementation(sub_ps, scene, ob_ref, psys, md, gpu_material);
}

blender::gpu::Batch *hair_sub_pass_setup(PassSimple::Sub &sub_ps,
                                         const Scene *scene,
                                         const ObjectRef &ob_ref,
                                         ParticleSystem *psys,
                                         ModifierData *md,
                                         GPUMaterial *gpu_material)
{
  return hair_sub_pass_setup_implementation(sub_ps, scene, ob_ref, psys, md, gpu_material);
}

}  // namespace blender::draw
