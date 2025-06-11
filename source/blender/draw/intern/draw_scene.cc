/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "draw_scene.hh"

#include "BKE_layer.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_view3d_types.h"
#include "DRW_render.hh"
#include "draw_handle.hh"

namespace blender::draw {

void foreach_obref_in_scene(DRWContext &draw_ctx, std::function<void(ObjectRef &)> callback)
{
  Depsgraph *depsgraph = draw_ctx.depsgraph;
  View3D *v3d = draw_ctx.v3d;

  DEGObjectIterSettings deg_iter_settings = {nullptr};
  deg_iter_settings.depsgraph = depsgraph;
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
    blender::draw::ObjectRef ob_ref(data_, ob);
    callback(ob_ref);
  }
  DEG_OBJECT_ITER_END;
}

}  // namespace blender::draw
