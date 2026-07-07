/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#include "draw_manager_text.hh"

#include "overlay_name.hh"

namespace blender::draw::overlay {

void Names::begin_sync(Resources &res, const State &state)
{
  enabled_ = state.is_space_v3d() && !res.is_selection();
  enabled_ &= state.show_text;

  if (!enabled_) {
    return;
  }
}

void Names::object_sync(Manager & /*manager*/,
                        const ObjectRef &ob_ref,
                        Resources &res,
                        const State &state)
{
  if (!enabled_) {
    return;
  }

  Object *ob = ob_ref.object;

  if (is_from_dupli_or_set(ob)) {
    return;
  }

  if ((ob->dtx & OB_DRAWNAME) == 0) {
    return;
  }

  ThemeColorID theme_id = res.object_wire_theme_id(ob_ref, state);

  uchar color[4];
  /* Color Management: Exception here as texts are drawn in sRGB space directly. */
  ui::theme::get_color_4ubv(theme_id, color);

  DRW_text_cache_add(state.dt,
                     ob->object_to_world().location(),
                     ob->id.name + 2,
                     strlen(ob->id.name + 2),
                     10,
                     0,
                     DRW_TEXT_CACHE_GLOBALSPACE | DRW_TEXT_CACHE_STRING_PTR,
                     color);
}

}  // namespace blender::draw::overlay
