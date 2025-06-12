/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_scene.hh"

#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_view3d_types.h"
#include "DRW_render.hh"
#include "draw_handle.hh"
#include "draw_view_data.hh"

/* TODO: De-duplicat with DEG code. */
static void ensure_id_properties_freed(const Object *dupli_object, Object *temp_dupli_object)
{
  if (temp_dupli_object->id.properties == nullptr) {
    /* No ID properties in temp data-block -- no leak is possible. */
    return;
  }
  if (temp_dupli_object->id.properties == dupli_object->id.properties) {
    /* Temp copy of object did not modify ID properties. */
    return;
  }
  /* Free memory which is owned by temporary storage which is about to get overwritten. */
  IDP_FreeProperty(temp_dupli_object->id.properties);
  temp_dupli_object->id.properties = nullptr;
}

namespace blender::draw {

void foreach_obref_in_scene(DRWContext &draw_ctx, std::function<void(ObjectRef &)> callback)
{
  Map<DrawObjectKey, DrawInstances> instances_map;

  Depsgraph *depsgraph = draw_ctx.depsgraph;
  View3D *v3d = draw_ctx.v3d;

  DEGObjectIterSettings deg_iter_settings = {nullptr};
  deg_iter_settings.depsgraph = depsgraph;
  /* TODO: Skip dupli expansion. */
  deg_iter_settings.flags = DEG_OBJECT_ITER_FOR_RENDER_ENGINE_FLAGS;
  if (v3d->flag2 & V3D_SHOW_VIEWER) {
    deg_iter_settings.viewer_path = &v3d->viewer_path;
  }
  DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, ob) {
    if ((v3d->object_type_exclude_viewport & (1 << ob->type)) != 0) {
      continue;
    }
    if (!BKE_object_is_visible_in_viewport(v3d, ob)) {
      continue;
    }

    ObjectRef ob_ref(data_, ob);
    if (!data_.dupli_parent) {
      callback(ob_ref);
      continue;
    }

    Object *dupli_parent = data_.dupli_parent;
    DupliObject *dupli = data_.dupli_object_current;

    if (!dupli) {
      /* Why can this happen? */
      continue;
    }

    uint64_t last_update = draw_ctx.view_data_active->depsgraph_last_update;

    DrawObjectFlags flags = DrawObjectFlags(0);
    {
      SET_FLAG_FROM_TEST(flags, dupli_parent == draw_ctx.obact, DrawObjectFlags::IsActive);
      SET_FLAG_FROM_TEST(flags, ob->trackflag & OB_NEG_SCALE, DrawObjectFlags::IsNegativeScale);
      SET_FLAG_FROM_TEST(flags,
                         ob->runtime->last_update_transform > last_update,
                         DrawObjectFlags::RecalcTransform);
      SET_FLAG_FROM_TEST(
          flags, ob->runtime->last_update_geometry > last_update, DrawObjectFlags::RecalcGeometry);
      SET_FLAG_FROM_TEST(
          flags, ob->runtime->last_update_shading > last_update, DrawObjectFlags::RecalcShading);
      SET_FLAG_FROM_TEST(flags, false /*TODO*/, DrawObjectFlags::ParentInEditPaintMode);
    }
    DrawObjectKey key(data_.dupli_object_current->ob,
                      data_.dupli_object_current->ob_data,
                      dupli_parent->base_flag,
                      flags,
                      ob->dt,
                      dupli_parent->light_linking,
                      dupli->preview_base_geometry,
                      dupli->preview_instance_index);

    DrawInstances &instances = instances_map.lookup_or_add_default(key);
    instances.object_to_world.append(float4x4(dupli->mat));
    if (ob->particlesystem.first) {
      instances.particles_object_to_world.append(ob_ref.particles_matrix());
    }
    instances.persistent_id.append({});
    memcpy(
        instances.persistent_id.last().data(), dupli->persistent_id, sizeof(dupli->persistent_id));
    instances.random_id.append(dupli->random_id);
    instances.select_id.append(ob->runtime->select_id);
  }
  DEG_OBJECT_ITER_END;

  Object tmp_object;
  ObjectRuntimeHandle tmp_runtime;

  for (const auto &[key, instances] : instances_map.items()) {
    tmp_object = blender::dna::shallow_copy(*key.object);
    tmp_object.runtime = &tmp_runtime;
    *tmp_object.runtime = *key.object->runtime;

    tmp_object.base_flag = key.base_flags | BASE_FROM_DUPLI;
    /* Duplicated elements shouldn't care whether their original collection is visible or not. */
    tmp_object.base_flag |= BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT;

    /* TODO? Already checked before being added to the instances map */
    // tmp_object.base_local_view_bits = dupli_parent->base_local_view_bits;
    // tmp_object.runtime->local_collections_bits = dupli_parent->runtime->local_collections_bits;

    tmp_object.dt = key.draw_type;
    // copy_v4_v4(tmp_object.color, dupli_parent->color); /* TODO */
    /* Should use DrawInstances data instead. */
    tmp_object.runtime->select_id = -1;
    if (key.object->data != key.ob_data) {
      BKE_object_replace_data_on_shallow_copy(&tmp_object, key.ob_data);
    }

    /* This could be avoided by refactoring make_dupli() in order to track all negative scaling
     * recursively. */
    SET_FLAG_FROM_TEST(
        tmp_object.transflag, bool(key.flags & DrawObjectFlags::IsNegativeScale), OB_NEG_SCALE);

    /* Should use DrawInstances data instead. */
    tmp_object.runtime->object_to_world = float4x4();
    tmp_object.runtime->world_to_object = float4x4();

    // BLI_assert(deg::deg_validate_eval_copy_datablock(&tmp_object.id)); TODO?

    blender::draw::ObjectRef ob_ref(tmp_object, key, instances);
    callback(ob_ref);

    ensure_id_properties_freed(key.object, &tmp_object);
  }
}

}  // namespace blender::draw
