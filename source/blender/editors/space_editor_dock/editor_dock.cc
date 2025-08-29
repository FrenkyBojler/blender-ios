/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup speditordock
 */

#include "BKE_screen.hh"

#include "BLI_listbase.h"

#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_screen.hh"

#include "UI_interface.hh"

#include "WM_api.hh"

#include "ED_editor_dock.hh"

namespace blender::ed::editor_dock {

ScrArea *add_docked_area(bScreen *screen, const rcti &area_rect)
{
  ScrArea *docked_area = ED_screen_area_add_empty(screen, area_rect);
  BKE_screen_remove_double_scrverts(screen);
  docked_area->docked = MEM_callocN<ScrDockedAreaData>(__func__);
  return docked_area;
}

/* TODO this isn't editor dock specific. Move somewhere else? */
SpaceLink *add_docked_space(ScrArea *area,
                            const eSpace_Type type,
                            std::optional<int> subtype,
                            const Scene *scene)
{
  SpaceType *st = BKE_spacetype_from_id(type);
  if (!st) {
    return nullptr;
  }

  SpaceLink *sl_old = static_cast<SpaceLink *>(area->spacedata.first);

  area->spacetype = type;
  area->type = st;

  SpaceLink *sl = st->create(area, scene);
  BLI_addhead(&area->spacedata, sl);
  BLI_addhead(&area->docked_spaces_ordered, BLI_genericNodeN(sl));

  if (sl_old) {
    sl_old->regionbase = area->regionbase;
  }
  area->regionbase = sl->regionbase;
  BLI_listbase_clear(&sl->regionbase);

  if (st->space_subtype_item_extend && subtype) {
    st->space_subtype_set(area, *subtype);
  }

  return sl;
}

void activate_docked_space(bContext *C, ScrArea *docked_area, SpaceLink *space)
{
  BLI_assert(docked_area->docked);

  SpaceType *st = BKE_spacetype_from_id(space->spacetype);
  if (!st) {
    return;
  }

  ED_area_exit(C, docked_area);
  docked_area->spacetype = space->spacetype;
  docked_area->type = st;

  SpaceLink *sl_old = static_cast<SpaceLink *>(docked_area->spacedata.first);
  if (sl_old) {
    sl_old->regionbase = docked_area->regionbase;
  }
  docked_area->regionbase = space->regionbase;
  BLI_listbase_clear(&space->regionbase);
  BLI_remlink(&docked_area->spacedata, space);
  BLI_addhead(&docked_area->spacedata, space);

  ED_area_init(C, CTX_wm_window(C), docked_area);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_CHANGED, docked_area);
  ED_area_tag_refresh(docked_area);
  ED_area_tag_redraw(docked_area);

  WM_event_add_mousemove(CTX_wm_window(C));
}

#if 0
static void visibility_changed(bContext *C, bScreen *screen, ScrArea *docked_area)
{
  /* TODO how much of this is actually needed? */
  ED_area_init(C, CTX_wm_window(C), docked_area);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_CHANGED, docked_area);
  ED_area_tag_refresh(docked_area);
  ED_area_tag_redraw(docked_area);
  WM_event_add_mousemove(CTX_wm_window(C));
}
#endif

void toggle_docked_space(bContext *C, ScrArea *docked_area, SpaceLink *space)
{
  BLI_assert(BLI_findindex(&docked_area->spacedata, space) >= 0);

  const wmWindow *win = CTX_wm_window(C);
  SpaceLink *sl_old = static_cast<SpaceLink *>(docked_area->spacedata.first);
  const bool is_visible = (docked_area->flag & AREA_FLAG_HIDDEN) == 0;
  const bool change_space = sl_old != space;

  if (change_space) {
    activate_docked_space(C, docked_area, space);
  }

  if (is_visible && change_space) {
    /* Pass. Just switching editors. */
  }
  else if (is_visible) {
    hide_docked_area(win, docked_area);
  }
  else {
    unhide_docked_area(win, docked_area);
  }
}

void hide_docked_area(const wmWindow *win, ScrArea *docked_area)
{
  BLI_assert(docked_area->docked);
  if ((docked_area->flag & AREA_FLAG_HIDDEN) != 0) {
    /* Already hidden. */
    return;
  }

  bScreen *screen = WM_window_get_active_screen(win);

  rcti screen_rect;
  WM_window_screen_rect_calc(win, &screen_rect);

  docked_area->flag |= AREA_FLAG_HIDDEN;

  ED_screen_area_geometry_detatch(screen, docked_area);

  /* Make area have a width of 0, so it's ignored and invisible for sure. */
  docked_area->v1->vec.x = docked_area->v2->vec.x = screen_rect.xmax - 1;
  docked_area->v1->vec.y = docked_area->v4->vec.y = screen_rect.ymin;
  docked_area->v3->vec.x = docked_area->v4->vec.x = screen_rect.xmax - 1;
  docked_area->v2->vec.y = docked_area->v3->vec.y = screen_rect.ymax - 1;

  screen->do_refresh = true;
}

void unhide_docked_area(const wmWindow *win, ScrArea *docked_area)
{
  BLI_assert(docked_area->docked);
  if ((docked_area->flag & AREA_FLAG_HIDDEN) == 0) {
    /* Already unhidden. */
    return;
  }

  bScreen *screen = WM_window_get_active_screen(win);
  const int width = UI_UNIT_X * 16;

  rcti screen_rect;
  WM_window_screen_rect_calc(win, &screen_rect);

  docked_area->flag &= ~AREA_FLAG_HIDDEN;

  /* Detach area geometry to edit it independently. */
  ED_screen_area_geometry_detatch(screen, docked_area);

  /* Move screen geometry outside the screen rect bounds, to force scaling areas to fit the
   * docked one. */
  docked_area->v1->vec.x = docked_area->v2->vec.x = screen_rect.xmax - 1;
  docked_area->v3->vec.x = docked_area->v4->vec.x = screen_rect.xmax - 1 + width;
  docked_area->v1->vec.y = docked_area->v4->vec.y = screen_rect.ymin;
  docked_area->v2->vec.y = docked_area->v3->vec.y = screen_rect.ymax - 1;
  /* Re-attach area geometry. */
  BKE_screen_remove_double_scrverts(screen);

  screen->do_refresh = true;
}

}  // namespace blender::ed::editor_dock
