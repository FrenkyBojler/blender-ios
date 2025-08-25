/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_scene.hh"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.h"
#include "BLI_sys_types.h"
#include "BLI_utildefines.h"

#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_particle.h"
#include "BKE_scene.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_view3d_types.h"

#include "DRW_render.hh"
#include "draw_handle.hh"
#include "draw_view_data.hh"

namespace blender::draw {

static bool supports_handle_ranges(Object *ob)
{
  if (ob->type == OB_MESH) {
    /* Hair drawing doesn't support handle ranges. */
    LISTBASE_FOREACH (ParticleSystem *, psys, &ob->particlesystem) {
      const int draw_as = (psys->part->draw_as == PART_DRAW_REND) ? psys->part->ren_as :
                                                                    psys->part->draw_as;
      if (draw_as == PART_DRAW_PATH && DRW_object_is_visible_psys_in_active_context(ob, psys)) {
        return false;
      }
    }
    /* Smoke drawing doesn't support handle ranges. */
    return !BKE_modifiers_findby_type(ob, eModifierType_Fluid);
  }
  return ELEM(ob->type, OB_CURVES_LEGACY, OB_SURF, OB_FONT, OB_POINTCLOUD, OB_GREASE_PENCIL);
}

enum class InstancesFlags : uint8_t {
  IsNegativeScale = 1 << 0,
};
ENUM_OPERATORS(InstancesFlags, InstancesFlags::IsNegativeScale);

struct InstancesKey {
  uint64_t hash_value;

  Object *object;
  ID *ob_data;
  const blender::bke::GeometrySet *preview_base_geometry;
  int preview_instance_index;
  InstancesFlags flags;

  InstancesKey(Object *object,
               ID *ob_data,
               InstancesFlags flags,
               const blender::bke::GeometrySet *preview_base_geometry,
               int preview_instance_index)
      : object(object),
        ob_data(ob_data),
        preview_base_geometry(preview_base_geometry),
        preview_instance_index(preview_instance_index),
        flags(flags)
  {
    hash_value = get_default_hash(object);
    hash_value = get_default_hash(hash_value, ob_data);
    hash_value = get_default_hash(hash_value, preview_base_geometry);
    hash_value = get_default_hash(hash_value, preview_instance_index);
    hash_value = get_default_hash(hash_value, uint8_t(flags));
  }

  uint64_t hash() const
  {
    return hash_value;
  }

  bool operator<(const InstancesKey &k) const
  {
    if (hash_value != k.hash_value) {
      return hash_value < k.hash_value;
    }
    if (object != k.object) {
      return object < k.object;
    }
    if (ob_data != k.ob_data) {
      return ob_data < k.ob_data;
    }
    if (flags != k.flags) {
      return flags < k.flags;
    }
    if (preview_base_geometry != k.preview_base_geometry) {
      return preview_base_geometry < k.preview_base_geometry;
    }
    if (preview_instance_index != k.preview_instance_index) {
      return preview_instance_index < k.preview_instance_index;
    }
    return false;
  }

  bool operator==(const InstancesKey &k) const
  {
    if (hash_value != k.hash_value) {
      return false;
    }
    if (object != k.object) {
      return false;
    }
    if (ob_data != k.ob_data) {
      return false;
    }
    if (flags != k.flags) {
      return false;
    }
    if (preview_base_geometry != k.preview_base_geometry) {
      return false;
    }
    if (preview_instance_index != k.preview_instance_index) {
      return false;
    }
    return true;
  }
};

void foreach_obref_in_scene(DRWContext &draw_ctx,
                            FunctionRef<bool(Object &)> should_draw_object_cb,
                            FunctionRef<void(ObjectRef &)> draw_object_cb)
{
  DupliList duplilist;
  Map<InstancesKey, VectorList<DupliObject *>> dupli_map;

  Object tmp_object;
  ObjectRuntimeHandle tmp_runtime;

  Depsgraph *depsgraph = draw_ctx.depsgraph;
  eEvaluationMode eval_mode = DEG_get_mode(depsgraph);
  View3D *v3d = draw_ctx.v3d;

  /* EEVEE is not supported for now. */
  const bool engines_support_handle_ranges = (v3d && v3d->shading.type <= OB_SOLID) ||
                                             BKE_scene_uses_blender_workbench(draw_ctx.scene);

  DEGObjectIterSettings deg_iter_settings = {nullptr};
  deg_iter_settings.depsgraph = depsgraph;
  deg_iter_settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY |
                            DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET;
  if (v3d && v3d->flag2 & V3D_SHOW_VIEWER) {
    deg_iter_settings.viewer_path = &v3d->viewer_path;
  }

  DEG_OBJECT_ITER_BEGIN (&deg_iter_settings, ob) {

    if (!DEG_iterator_object_is_visible(eval_mode, ob)) {
      continue;
    }

    int visibility = BKE_object_visibility(ob, eval_mode);
    bool ob_visible = visibility & (OB_VISIBLE_SELF | OB_VISIBLE_PARTICLES);

    if (ob_visible && should_draw_object_cb(*ob)) {
      ObjectRef ob_ref(ob);
      draw_object_cb(ob_ref);
    }

    bool instances_visible = (visibility & OB_VISIBLE_INSTANCES) &&
                             ((ob->transflag & OB_DUPLI) ||
                              ob->runtime->geometry_set_eval != nullptr);

    if (!instances_visible) {
      continue;
    }

    duplilist.clear();
    object_duplilist(
        draw_ctx.depsgraph, draw_ctx.scene, ob, deg_iter_settings.included_objects, duplilist);

    if (duplilist.is_empty()) {
      continue;
    }

    dupli_map.clear();
    for (DupliObject &dupli : duplilist) {

      if (!DEG_iterator_dupli_is_visible(&dupli, eval_mode)) {
        continue;
      }

      /* TODO: Optimize.
       * We can't check the dupli.ob since visibility may be different than the dupli itself.
       * But we should be able to check the dupli visibility without creating a temp object. */
#if 0
      if (!should_draw_object_cb(*dupli.ob)) {
        continue;
      }
#endif

      if (!engines_support_handle_ranges || !supports_handle_ranges(dupli.ob)) {
        /* Sync the dupli as a single object. */
        if (!evil::DEG_iterator_temp_object_from_dupli(
                ob, &dupli, eval_mode, false, &tmp_object, &tmp_runtime) ||
            !should_draw_object_cb(tmp_object))
        {
          evil::DEG_iterator_temp_object_free_properties(&dupli, &tmp_object);
          continue;
        }

        tmp_object.light_linking = ob->light_linking;
        SET_FLAG_FROM_TEST(tmp_object.transflag, is_negative_m4(dupli.mat), OB_NEG_SCALE);
        tmp_object.runtime->object_to_world = float4x4(dupli.mat);
        tmp_object.runtime->world_to_object = invert(tmp_object.runtime->object_to_world);

        blender::draw::ObjectRef ob_ref(&tmp_object, ob, &dupli);
        draw_object_cb(ob_ref);

        evil::DEG_iterator_temp_object_free_properties(&dupli, &tmp_object);
        continue;
      }

      InstancesFlags flags = InstancesFlags(0);
      {
        SET_FLAG_FROM_TEST(flags, is_negative_m4(dupli.mat), InstancesFlags::IsNegativeScale);
      }
      InstancesKey key(dupli.ob,
                       dupli.ob_data,
                       flags,
                       dupli.preview_base_geometry,
                       dupli.preview_instance_index);

      dupli_map.lookup_or_add_default(key).append(&dupli);
    }

    for (const auto &[key, instances] : dupli_map.items()) {
      DupliObject *first_dupli = instances.first();
      if (!evil::DEG_iterator_temp_object_from_dupli(
              ob, first_dupli, eval_mode, false, &tmp_object, &tmp_runtime) ||
          !should_draw_object_cb(tmp_object))
      {
        evil::DEG_iterator_temp_object_free_properties(first_dupli, &tmp_object);
        continue;
      }

      tmp_object.light_linking = ob->light_linking;
      SET_FLAG_FROM_TEST(
          tmp_object.transflag, bool(key.flags & InstancesFlags::IsNegativeScale), OB_NEG_SCALE);
      /* Should use DrawInstances data instead. */
      tmp_object.runtime->object_to_world = float4x4();
      tmp_object.runtime->world_to_object = float4x4();

      blender::draw::ObjectRef ob_ref(tmp_object, ob, instances);
      draw_object_cb(ob_ref);

      evil::DEG_iterator_temp_object_free_properties(first_dupli, &tmp_object);
    }
  }
  DEG_OBJECT_ITER_END;
}

}  // namespace blender::draw
