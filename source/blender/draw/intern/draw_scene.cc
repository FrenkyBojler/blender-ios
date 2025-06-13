/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_scene.hh"

#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "DEG_depsgraph_query.hh"
#include "DNA_view3d_types.h"
#include "DRW_render.hh"
#include "draw_handle.hh"
#include "draw_view_data.hh"

namespace blender::draw {

void foreach_obref_in_scene(DRWContext &draw_ctx, std::function<void(ObjectRef &)> callback)
{
  DupliList duplilist;
  Map<DrawObjectKey, VectorList<DupliObject *>> dupli_map;

  Object tmp_object;
  ObjectRuntimeHandle tmp_runtime;

  const uint64_t last_update = draw_ctx.view_data_active->depsgraph_last_update;

  Depsgraph *depsgraph = draw_ctx.depsgraph;
  eEvaluationMode eval_mode = DEG_get_mode(depsgraph);
  View3D *v3d = draw_ctx.v3d;

  DEGObjectIterSettings deg_iter_settings = {nullptr};
  deg_iter_settings.depsgraph = depsgraph;
  /* TODO: Skip dupli expansion. */
  deg_iter_settings.flags = DEG_ITER_OBJECT_FLAG_LINKED_DIRECTLY |
                            DEG_ITER_OBJECT_FLAG_LINKED_VIA_SET | DEG_ITER_OBJECT_FLAG_VISIBLE;
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

    {
      ObjectRef ob_ref(data_, ob);
      callback(ob_ref);
    }
    if (!data_.dupli_parent) {
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

      if (DEG_iterator_should_skip_dupli(eval_mode, &dupli)) {
        continue;
      }

      DrawObjectFlags flags = DrawObjectFlags(0);
      {
        SET_FLAG_FROM_TEST(flags, is_negative_m4(dupli.mat), DrawObjectFlags::IsNegativeScale);
        SET_FLAG_FROM_TEST(flags,
                           ob->runtime->last_update_transform > last_update ||
                               dupli.ob->runtime->last_update_transform > last_update,
                           DrawObjectFlags::RecalcTransform);
        SET_FLAG_FROM_TEST(flags,
                           ob->runtime->last_update_geometry > last_update ||
                               dupli.ob->runtime->last_update_geometry > last_update,
                           DrawObjectFlags::RecalcGeometry);
        SET_FLAG_FROM_TEST(flags,
                           ob->runtime->last_update_shading > last_update ||
                               dupli.ob->runtime->last_update_shading > last_update,
                           DrawObjectFlags::RecalcShading);
      }
      DrawObjectKey key(dupli.ob,
                        dupli.ob_data,
                        flags,
                        dupli.preview_base_geometry,
                        dupli.preview_instance_index);

      dupli_map.lookup_or_add_default(key).append(&dupli);
    }

    for (const auto &[key, instances] : dupli_map.items()) {
      if (!DEG_iterator_setup_temp_object(
              ob, key.object, key.ob_data, &tmp_object, &tmp_runtime, eval_mode))
      {
        DEG_iterator_free_temp_object_properties(key.object, &tmp_object);
        continue;
      }

      tmp_object.light_linking = ob->light_linking;

      SET_FLAG_FROM_TEST(
          tmp_object.transflag, bool(key.flags & DrawObjectFlags::IsNegativeScale), OB_NEG_SCALE);

      /* Should use DrawInstances data instead. */
      tmp_object.runtime->object_to_world = float4x4();
      tmp_object.runtime->world_to_object = float4x4();

      blender::draw::ObjectRef ob_ref(tmp_object, ob, key, instances);
      callback(ob_ref);

      DEG_iterator_free_temp_object_properties(key.object, &tmp_object);
    }
  }
  DEG_OBJECT_ITER_END;
}

}  // namespace blender::draw
