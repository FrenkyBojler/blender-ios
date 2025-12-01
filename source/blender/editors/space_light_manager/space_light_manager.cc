/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup splightmanager
 */

 #include <cstring>

 #include "MEM_guardedalloc.h"

 #include "BLI_listbase.h"
 #include "BLI_string_utf8.h"

 #include "BLT_translation.hh"

 #include "BKE_screen.hh"

 #include "ED_screen.hh"
 #include "ED_space_api.hh"

 #include "WM_api.hh"
 #include "WM_message.hh"
 #include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "BLO_read_write.hh"

#include "DNA_space_types.h"

namespace blender::ed::light_manager {

struct SpaceLightManager_Runtime {
};

/* -------------------------------------------------------------------- */
/** \name Main Region
 * \{ */

static void light_manager_main_region_init(wmWindowManager *wm, ARegion *region)
{
  region->v2d.scroll = V2D_SCROLL_RIGHT | V2D_SCROLL_BOTTOM | V2D_SCROLL_VERTICAL_HIDE |
                       V2D_SCROLL_HORIZONTAL_HIDE;
  region->v2d.align = V2D_ALIGN_NO_NEG_X | V2D_ALIGN_NO_POS_Y;
  region->v2d.keepzoom = V2D_LOCKZOOM_X | V2D_LOCKZOOM_Y | V2D_LIMITZOOM | V2D_KEEPASPECT;
  region->v2d.keeptot = V2D_KEEPTOT_STRICT;
  region->v2d.minzoom = region->v2d.maxzoom = 1.0f;

  UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_LIST, region->winx, region->winy);

  region->flag |= RGN_FLAG_INDICATE_OVERFLOW;

  wmKeyMap *keymap = WM_keymap_ensure(
      wm->runtime->defaultconf, "Light Manager", SPACE_LIGHT_MANAGER, RGN_TYPE_WINDOW);
  WM_event_add_keymap_handler(&region->runtime->handlers, keymap);
}

static void light_manager_main_region_draw(const bContext *C, ARegion *region)
{
  UI_ThemeClearColor(TH_BACK);

  uiBlock *block = UI_block_begin(C, region, __func__, ui::EmbossType::None);
  const uiStyle *style = UI_style_get_dpi();

  uiLayout &layout = ui::block_layout(block,
                                      ui::LayoutDirection::Vertical,
                                      ui::LayoutType::Panel,
                                      0,
                                      0,
                                      region->winx,
                                      region->winy,
                                      0,
                                      style);

  layout.label(IFACE_("Light Manager"), ICON_LIGHT_DATA);
  layout.label(IFACE_("Work in progress"), ICON_NONE);

  ui::block_layout_resolve(block);
  UI_block_end(C, block);
  UI_block_draw(C, block);

  ED_region_draw_overflow_indication(CTX_wm_area(C), region);
}

static void light_manager_main_region_listener(const wmRegionListenerParams * /*params*/)
{
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Header Region
 * \{ */

static void light_manager_header_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  ED_region_header_init(region);
}

static void light_manager_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

static void light_manager_header_region_listener(const wmRegionListenerParams * /*params*/)
{
}

/* \} */

/* -------------------------------------------------------------------- */
/** \name Space Type Callbacks
 * \{ */

static SpaceLink *light_manager_create(const ScrArea * /*area*/, const Scene * /*scene*/)
{
  SpaceLightManager *space_lm = MEM_callocN<SpaceLightManager>("init light manager space");
  space_lm->spacetype = SPACE_LIGHT_MANAGER;
  space_lm->runtime = MEM_new<SpaceLightManager_Runtime>(__func__);

  ARegion *region;

  /* Header. */
  region = BKE_area_region_new();
  BLI_addtail(&space_lm->regionbase, region);
  region->regiontype = RGN_TYPE_HEADER;
  region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;

  /* Main window. */
  region = BKE_area_region_new();
  BLI_addtail(&space_lm->regionbase, region);
  region->regiontype = RGN_TYPE_WINDOW;

  return (SpaceLink *)space_lm;
}

static void light_manager_free(SpaceLink *sl)
{
  SpaceLightManager *space_lm = (SpaceLightManager *)sl;

  MEM_delete(space_lm->runtime);
}

static void light_manager_init(wmWindowManager * /*wm*/, ScrArea * /*area*/)
{
}

static SpaceLink *light_manager_duplicate(SpaceLink *sl)
{
  SpaceLightManager *space_lm = (SpaceLightManager *)sl;
  SpaceLightManager *space_lm_new = MEM_dupallocN<SpaceLightManager>(__func__, *space_lm);
  space_lm_new->runtime = MEM_new<SpaceLightManager_Runtime>(__func__);

  return (SpaceLink *)space_lm_new;
}

static void light_manager_space_blend_read_data(BlendDataReader * /*reader*/, SpaceLink *sl)
{
  SpaceLightManager *space_lm = (SpaceLightManager *)sl;
  space_lm->runtime = MEM_new<SpaceLightManager_Runtime>(__func__);
}

static void light_manager_space_blend_write(BlendWriter *writer, SpaceLink *sl)
{
  BLO_write_struct(writer, SpaceLightManager, sl);
}

/* \} */

}  // namespace blender::ed::light_manager

void ED_spacetype_light_manager()
{
  using namespace blender::ed::light_manager;

  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();
  ARegionType *art;

  st->spaceid = SPACE_LIGHT_MANAGER;
  STRNCPY_UTF8(st->name, "Light Manager");

  st->create = light_manager_create;
  st->free = light_manager_free;
  st->init = light_manager_init;
  st->duplicate = light_manager_duplicate;
  st->blend_read_data = light_manager_space_blend_read_data;
  st->blend_read_after_liblink = nullptr;
  st->blend_write = light_manager_space_blend_write;

  /* Main region. */
  art = MEM_callocN<ARegionType>("spacetype light manager region");
  art->regionid = RGN_TYPE_WINDOW;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D;

  art->init = light_manager_main_region_init;
  art->draw = light_manager_main_region_draw;
  art->listener = light_manager_main_region_listener;
  BLI_addhead(&st->regiontypes, art);

  /* Header. */
  art = MEM_callocN<ARegionType>("spacetype light manager header region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;

  art->init = light_manager_header_region_init;
  art->draw = light_manager_header_region_draw;
  art->listener = light_manager_header_region_listener;
  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(std::move(st));
}
