/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup eevee
 *
 * Converts the different renderable object types to draw-calls.
 */

#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "DNA_curves_types.h"
#include "DNA_modifier_types.h"
#include "DNA_particle_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_volume_types.h"

#include "draw_cache.hh"
#include "draw_common.hh"
#include "draw_sculpt.hh"

#include "eevee_instance.hh"

namespace blender::eevee {

/* -------------------------------------------------------------------- */
/** \name Recalc
 *
 * \{ */

ObjectHandle SyncModule::sync_object(const ObjectRef &ob_ref,
                                     const ResourceHandleRange &res_handle,
                                     uint sub_key)
{
  return ObjectHandle(ob_ref, res_handle, inst_.get_recalc_flags(ob_ref), sub_key);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Common
 * \{ */

static inline void geometry_call(PassMain::Sub *sub_pass,
                                 gpu::Batch *geom,
                                 ResourceHandleRange resource_handle)
{
  if (sub_pass != nullptr) {
    sub_pass->draw(geom, resource_handle);
  }
}

static inline void volume_call(PassMain::Sub *pass,
                               GPUMaterial *gpumat,
                               Scene *scene,
                               Object *ob,
                               gpu::Batch *geom,
                               ResourceHandleRange res_handle)
{
  BLI_assert(res_handle.index_range().size() == 1);
  if (pass != nullptr) {
    /** WARNING:
     * drw_volume_object_mesh_init doesn't rely on per-object data,
     * but volume_object_grids_init does!
     * We're fine while Volume objects dont support handle ranges. */
    PassMain::Sub *object_pass = volume_sub_pass(*pass, scene, ob, gpumat);
    if (object_pass != nullptr) {
      object_pass->draw(geom, res_handle);
    }
  }
}

static inline void volume_call(MaterialPass &matpass,
                               Scene *scene,
                               Object *ob,
                               gpu::Batch *geom,
                               ResourceHandleRange res_handle)
{
  volume_call(matpass.sub_pass, matpass.gpumat, scene, ob, geom, res_handle);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mesh
 * \{ */

void SyncModule::sync_mesh(const ObjectRef &ob_ref)
{
  if (!inst_.use_surfaces) {
    return;
  }

  if ((ob_ref.object->dt < OB_SOLID) &&
      (inst_.is_viewport() && inst_.v3d->shading.type != OB_RENDER))
  {
    /** Do not render objects with display type lower than solid when in material preview mode. */
    return;
  }

  ObjectHandle ob_handle = sync_object(ob_ref, inst_.manager->unique_handle(ob_ref));

  bool has_motion = inst_.velocity.step_object_sync(ob_handle);

  MaterialSyncArray &material_array = inst_.materials.material_passes_sync(
      ob_handle,
      eMaterialGeometry::MAT_GEOM_MESH,
      has_motion,
      [&](GPUMaterial &gpumat, PassMain::Sub &pass) {
        gpu::Batch *geom = DRW_cache_object_surface_material_get(ob_handle.object,
                                                                 Span(&gpumat, 1));
      });

  Span<gpu::Batch *> mat_geom = DRW_cache_object_surface_material_get(
      ob_handle.object, material_array.gpu_materials);
  if (mat_geom.is_empty()) {
    return;
  }

  bool is_alpha_blend = false;
  bool has_transparent_shadows = false;
  bool has_volume = false;
  float inflate_bounds = 0.0f;
  for (auto i : material_array.gpu_materials.index_range()) {
    gpu::Batch *geom = mat_geom[i];
    if (geom == nullptr) {
      continue;
    }

    MaterialSync &material = material_array.materials[i];
    GPUMaterial *gpu_material = material_array.gpu_materials[i];

    if (material.has_volume) {
      has_volume = true;

      for (int instance : IndexRange(ob_handle.instances_count())) {
        volume_call(material.sub_pass_arrays->volume_occupancy_sub_passes[instance],
                    material.volume_occupancy.gpumat,
                    inst_.scene,
                    ob_handle.object,
                    geom,
                    ob_handle.res_handle.sub_handle(instance));
        volume_call(material.sub_pass_arrays->volume_material_sub_passes[instance],
                    material.volume_material.gpumat,
                    inst_.scene,
                    ob_handle.object,
                    geom,
                    ob_handle.res_handle.sub_handle(instance));
      }

      /* Do not render surface if we are rendering a volume object
       * and do not have a surface closure. */
      if (!material.has_surface) {
        continue;
      }
    }

    geometry_call(material.capture.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.prepass.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.shading.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.shadow.sub_pass, geom, ob_handle.res_handle);

    for (int i : material.sub_pass_arrays->overlap_masking_sub_passes.index_range()) {
      geometry_call(material.sub_pass_arrays->overlap_masking_sub_passes[i],
                    geom,
                    ob_handle.res_handle.sub_handle(i));
    }
    for (int i : material.sub_pass_arrays->shading_blend_transparent_sub_passes.index_range()) {
      geometry_call(material.sub_pass_arrays->shading_blend_transparent_sub_passes[i],
                    geom,
                    ob_handle.res_handle.sub_handle(i));
    }

    geometry_call(material.planar_probe_prepass.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.planar_probe_shading.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.lightprobe_sphere_prepass.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.lightprobe_sphere_shading.sub_pass, geom, ob_handle.res_handle);

    is_alpha_blend = is_alpha_blend || material.is_alpha_blend_transparent;
    has_transparent_shadows = has_transparent_shadows || material.has_transparent_shadows;

    blender::Material *mat = GPU_material_get_material(gpu_material);
    inst_.cryptomatte.sync_material(mat);

    if (GPU_material_has_displacement_output(gpu_material)) {
      inflate_bounds = math::max(inflate_bounds, mat->inflate_bounds);
    }
  }

  if (has_volume) {
    inst_.volume.object_sync(ob_handle);
  }

  if (inflate_bounds != 0.0f) {
    inst_.manager->update_handle_bounds(ob_handle.res_handle, ob_ref, inflate_bounds);
  }

  inst_.manager->extract_object_attributes(
      ob_handle.res_handle, ob_ref, material_array.gpu_materials);

  inst_.shadows.sync_object(ob_handle, is_alpha_blend, has_transparent_shadows);
  inst_.cryptomatte.sync_object(ob_handle);
}

bool SyncModule::sync_sculpt(const ObjectRef &ob_ref)
{
  if (!inst_.use_surfaces) {
    return false;
  }

  bool pbvh_draw = BKE_sculptsession_use_pbvh_draw(ob_ref.object, inst_.rv3d) &&
                   !inst_.is_image_render;
  if (!pbvh_draw) {
    return false;
  }

  ObjectHandle ob_handle = sync_object(ob_ref, inst_.manager->unique_handle_for_sculpt(ob_ref));

  bool has_motion = false;
  MaterialSyncArray &material_array = inst_.materials.material_array_get(ob_handle, has_motion);

  bool is_alpha_blend = false;
  bool has_transparent_shadows = false;
  bool has_volume = false;
  float inflate_bounds = 0.0f;
  for (SculptBatch &batch :
       sculpt_batches_per_material_get(ob_ref.object, material_array.gpu_materials))
  {
    gpu::Batch *geom = batch.batch;
    if (geom == nullptr) {
      continue;
    }

    Material &material = material_array.materials[batch.material_slot];

    if (material.has_volume) {
      volume_call(
          material.volume_occupancy, inst_.scene, ob_handle.object, geom, ob_handle.res_handle);
      volume_call(
          material.volume_material, inst_.scene, ob_handle.object, geom, ob_handle.res_handle);
      has_volume = true;
      /* Do not render surface if we are rendering a volume object
       * and do not have a surface closure. */
      if (material.has_surface == false) {
        continue;
      }
    }

    geometry_call(material.capture.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.overlap_masking.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.prepass.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.shading.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.shadow.sub_pass, geom, ob_handle.res_handle);

    geometry_call(material.planar_probe_prepass.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.planar_probe_shading.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.lightprobe_sphere_prepass.sub_pass, geom, ob_handle.res_handle);
    geometry_call(material.lightprobe_sphere_shading.sub_pass, geom, ob_handle.res_handle);

    is_alpha_blend = is_alpha_blend || material.is_alpha_blend_transparent;
    has_transparent_shadows = has_transparent_shadows || material.has_transparent_shadows;

    GPUMaterial *gpu_material = material_array.gpu_materials[batch.material_slot];
    blender::Material *mat = GPU_material_get_material(gpu_material);
    inst_.cryptomatte.sync_material(mat);

    if (GPU_material_has_displacement_output(gpu_material)) {
      inflate_bounds = math::max(inflate_bounds, mat->inflate_bounds);
    }
  }

  if (has_volume) {
    inst_.volume.object_sync(ob_handle);
  }

  inst_.manager->extract_object_attributes(
      ob_handle.res_handle, ob_handle, material_array.gpu_materials);

  inst_.shadows.sync_object(ob_handle, is_alpha_blend, has_transparent_shadows);
  inst_.cryptomatte.sync_object(ob_handle);

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Point Cloud
 * \{ */

void SyncModule::sync_pointcloud(const ObjectRef &ob_ref)
{
  const int material_slot = POINTCLOUD_MATERIAL_NR;

  ObjectHandle ob_handle = sync_object(ob_ref, inst_.manager->unique_handle(ob_ref));

  bool has_motion = inst_.velocity.step_object_sync(ob_handle);

  MaterialSync material = inst_.materials.material_get(
      ob_handle, has_motion, material_slot - 1, MAT_GEOM_POINTCLOUD);

  auto drawcall_add = [&](MaterialPass &matpass, bool dual_sided = false) {
    if (matpass.sub_pass == nullptr) {
      return;
    }
    PassMain::Sub &object_pass = matpass.sub_pass->sub("Point Cloud Sub Pass");
    gpu::Batch *geometry = pointcloud_sub_pass_setup(
        object_pass, ob_handle.object, matpass.gpumat);
    if (dual_sided) {
      /* WORKAROUND: Hack to generate backfaces. Should also be baked into the Index Buf too at
       * some point in the future. */
      object_pass.push_constant("ptcloud_backface", false);
      object_pass.draw(geometry, ob_handle.res_handle);
      object_pass.push_constant("ptcloud_backface", true);
      object_pass.draw(geometry, ob_handle.res_handle);
    }
    else {
      object_pass.push_constant("ptcloud_backface", false);
      object_pass.draw(geometry, ob_handle.res_handle);
    }
  };

  if (material.has_volume) {
    /* Only support single volume material for now. */
    drawcall_add(material.volume_occupancy, true);
    drawcall_add(material.volume_material);
    inst_.volume.object_sync(ob_handle);

    /* Do not render surface if we are rendering a volume object
     * and do not have a surface closure. */
    if (material.has_surface == false) {
      return;
    }
  }

  drawcall_add(material.capture);
  drawcall_add(material.overlap_masking);
  drawcall_add(material.prepass);
  drawcall_add(material.shading);
  drawcall_add(material.shadow);

  drawcall_add(material.planar_probe_prepass);
  drawcall_add(material.planar_probe_shading);
  drawcall_add(material.lightprobe_sphere_prepass);
  drawcall_add(material.lightprobe_sphere_shading);

  inst_.cryptomatte.sync_object(ob_handle);
  GPUMaterial *gpu_material = material.shading.gpumat;
  blender::Material *mat = GPU_material_get_material(gpu_material);
  inst_.cryptomatte.sync_material(mat);

  if (GPU_material_has_displacement_output(gpu_material) && mat->inflate_bounds != 0.0f) {
    inst_.manager->update_handle_bounds(ob_handle.res_handle, ob_ref, mat->inflate_bounds);
  }

  inst_.manager->extract_object_attributes(ob_handle.res_handle, ob_ref, material.shading.gpumat);

  inst_.shadows.sync_object(
      ob_handle, material.is_alpha_blend_transparent, material.has_transparent_shadows);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Volume Objects
 * \{ */

void SyncModule::sync_volume(const ObjectRef &ob_ref)
{
  if (!inst_.use_volumes) {
    return;
  }

  ObjectHandle ob_handle = sync_object(ob_ref, inst_.manager->unique_handle(ob_ref));

  const int material_slot = VOLUME_MATERIAL_NR;

  /* Motion is not supported on volumes yet. */
  const bool has_motion = false;

  MaterialSync material = inst_.materials.material_get(
      ob_handle, has_motion, material_slot - 1, MAT_GEOM_VOLUME);

  if (!GPU_material_has_volume_output(material.volume_material.gpumat)) {
    return;
  }

  /* Do not render the object if there is no attribute used in the volume.
   * This mimic Cycles behavior (see #124061). */
  ListBaseT<GPUMaterialAttribute> attr_list = GPU_material_attributes(
      material.volume_material.gpumat);
  if (BLI_listbase_is_empty(&attr_list)) {
    return;
  }

  auto drawcall_add =
      [&](MaterialPass &matpass, gpu::Batch *geom, ResourceHandleRange res_handle) {
        if (matpass.sub_pass == nullptr) {
          return false;
        }
        PassMain::Sub *object_pass = volume_sub_pass(
            *matpass.sub_pass, inst_.scene, ob_handle.object, matpass.gpumat);
        if (object_pass != nullptr) {
          object_pass->draw(geom, res_handle);
          return true;
        }
        return false;
      };

  /* Use bounding box tag empty spaces. */
  gpu::Batch *geom = inst_.volume.unit_cube_batch_get();

  bool is_rendered = false;
  is_rendered |= drawcall_add(material.volume_occupancy, geom, ob_handle.res_handle);
  is_rendered |= drawcall_add(material.volume_material, geom, ob_handle.res_handle);

  if (!is_rendered) {
    return;
  }

  inst_.manager->extract_object_attributes(
      ob_handle.res_handle, ob_handle, material.volume_material.gpumat);

  inst_.volume.object_sync(ob_handle);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hair
 * \{ */

void SyncModule::sync_curves(const ObjectRef &ob_ref, HairParticleInfo const *hair_particle)
{
  if (!inst_.use_curves) {
    return;
  }

  int mat_nr = CURVES_MATERIAL_NR;
  if (hair_particle != nullptr) {
    mat_nr = hair_particle->psys.part->omat;
  }

  ObjectHandle ob_handle = hair_particle ?
                               ObjectHandle(ob_ref,
                                            inst_.manager->resource_handle_for_psys(
                                                ob_ref, ob_ref.object_to_world()),
                                            hair_particle->recalc_flags,
                                            hair_particle->sub_key) :
                               sync_object(ob_ref, inst_.manager->unique_handle(ob_ref));

  bool has_motion = inst_.velocity.step_object_sync(ob_handle, hair_particle);
  MaterialSync material = inst_.materials.material_get(
      ob_handle, has_motion, mat_nr - 1, MAT_GEOM_CURVES);

  auto drawcall_add = [&](MaterialPass &matpass) {
    if (matpass.sub_pass == nullptr) {
      return;
    }
    if (hair_particle != nullptr) {
      PassMain::Sub &sub_pass = matpass.sub_pass->sub("Hair SubPass");
      gpu::Batch *geometry = hair_sub_pass_setup(
          sub_pass, inst_.scene, ob_ref, &hair_particle->psys, &hair_particle->md, matpass.gpumat);
      sub_pass.draw(geometry, ob_handle.res_handle);
    }
    else {
      PassMain::Sub &sub_pass = matpass.sub_pass->sub("Curves SubPass");
      const char *error = nullptr;
      gpu::Batch *geometry = curves_sub_pass_setup(
          sub_pass, inst_.scene, ob_ref.object, error, matpass.gpumat);
      if (error) {
        inst_.info_append(error);
      }
      sub_pass.draw(geometry, ob_handle.res_handle);
    }
  };

  if (material.has_volume) {
    /* Only support single volume material for now. */
    drawcall_add(material.volume_occupancy);
    drawcall_add(material.volume_material);
    inst_.volume.object_sync(ob_handle);
    /* Do not render surface if we are rendering a volume object
     * and do not have a surface closure. */
    if (material.has_surface == false) {
      return;
    }
  }

  drawcall_add(material.capture);
  drawcall_add(material.overlap_masking);
  drawcall_add(material.prepass);
  drawcall_add(material.shading);
  drawcall_add(material.shadow);

  drawcall_add(material.planar_probe_prepass);
  drawcall_add(material.planar_probe_shading);
  drawcall_add(material.lightprobe_sphere_prepass);
  drawcall_add(material.lightprobe_sphere_shading);

  inst_.cryptomatte.sync_object(ob_handle);
  GPUMaterial *gpu_material = material.shading.gpumat;
  blender::Material *mat = GPU_material_get_material(gpu_material);
  inst_.cryptomatte.sync_material(mat);

  if (GPU_material_has_displacement_output(gpu_material) && mat->inflate_bounds != 0.0f) {
    inst_.manager->update_handle_bounds(ob_handle.res_handle, ob_ref, mat->inflate_bounds);
  }

  inst_.manager->extract_object_attributes(ob_handle.res_handle, ob_ref, material.shading.gpumat);

  inst_.shadows.sync_object(
      ob_handle, material.is_alpha_blend_transparent, material.has_transparent_shadows);
}

/** \} */

void foreach_hair_particle(Instance &inst,
                           ObjectRef &ob_ref,
                           int instance_index,
                           HairHandleCallback callback)
{
  uint sub_key = 1;

  for (ModifierData &md : ob_ref.object->modifiers) {
    if (md.type == eModifierType_ParticleSystem) {
      ParticleSystem *particle_sys = reinterpret_cast<ParticleSystemModifierData *>(&md)->psys;
      ParticleSettings *part_settings = particle_sys->part;
      /* Only use the viewport drawing mode for material preview. */
      const int draw_as = (part_settings->draw_as == PART_DRAW_REND || !inst.is_viewport()) ?
                              part_settings->ren_as :
                              part_settings->draw_as;
      if (draw_as != PART_DRAW_PATH ||
          !DRW_object_is_visible_psys_in_active_context(ob_ref.object, particle_sys))
      {
        continue;
      }
      callback({md, *particle_sys, uint(particle_sys->recalc), sub_key++});
    }
  }
}

}  // namespace blender::eevee
