/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <cstring>
#include <fmt/format.h>

#include "BLI_listbase.h"
#include "BLI_string.h"

#include "BKE_screen.hh"

#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "DNA_space_types.h"

#include "MEM_guardedalloc.h"

#include "WM_api.hh"
#include "WM_types.hh"

#include "BLT_translation.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"

#include "BLO_read_write.hh"

#include <sstream>

static SpaceLink *project_create(const ScrArea * /*area*/, const Scene * /*scene*/)
{
  SpaceProject *project_space = MEM_callocN<SpaceProject>("project space");
  project_space->spacetype = SPACE_PROJECT;

  {
    /* Header. */
    ARegion *region = BKE_area_region_new();

    BLI_addtail(&project_space->regionbase, region);
    region->regiontype = RGN_TYPE_HEADER;
    region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;
  }

  {
    /* Main region. */
    ARegion *region = BKE_area_region_new();
    BLI_addtail(&project_space->regionbase, region);
    region->regiontype = RGN_TYPE_WINDOW;
  }

  {
    /* Navigation region. */
    ARegion *region = BKE_area_region_new();
    BLI_addtail(&project_space->regionbase, region);
    region->regiontype = RGN_TYPE_NAV_BAR;
    region->alignment = RGN_ALIGN_LEFT;
  }

  return (SpaceLink *)project_space;
}

static void project_free(SpaceLink * /*sl*/) {}

/* spacetype; init callback */
static void project_init(wmWindowManager * /*wm*/, ScrArea * /*area*/) {}

static SpaceLink *project_duplicate(SpaceLink *sl)
{
  SpaceProject *space_project = static_cast<SpaceProject *>(MEM_dupallocN(sl));

  /* clear or remove stuff from old */

  return (SpaceLink *)space_project;
}

/* add handlers, stuff you only do once or on area/region changes */
static void project_main_region_init(wmWindowManager *wm, ARegion *region)
{
  region->v2d.scroll = V2D_SCROLL_RIGHT | V2D_SCROLL_VERTICAL_HIDE;

  ED_region_panels_init(wm, region);
}

/* static void project_main_region_layout(const bContext *C, ARegion *region) {} */

static void project_operatortypes() {}

static void project_keymap(wmKeyConfig * /*keyconf*/) {}

/* add handlers, stuff you only do once or on area/region changes */
static void project_header_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  ED_region_header_init(region);
}

static void project_header_region_draw(const bContext *C, ARegion *region)
{
  ED_region_header(C, region);
}

/* add handlers, stuff you only do once or on area/region changes */
static void project_navigation_region_init(wmWindowManager *wm, ARegion *region)
{
  region->v2d.scroll = V2D_SCROLL_RIGHT | V2D_SCROLL_VERTICAL_HIDE;

  ED_region_panels_init(wm, region);
}

static void project_navigation_region_draw(const bContext *C, ARegion *region)
{
  ED_region_panels(C, region);
}

static bool project_execute_region_poll(const RegionPollParams *params)
{
  const ARegion *region_header = BKE_area_find_region_type(params->area, RGN_TYPE_HEADER);
  return !region_header->runtime->visible;
}

/* add handlers, stuff you only do once or on area/region changes */
static void project_execute_region_init(wmWindowManager *wm, ARegion *region)
{
  ED_region_panels_init(wm, region);
  region->v2d.keepzoom |= V2D_LOCKZOOM_X | V2D_LOCKZOOM_Y;
}

static void project_main_region_listener(const wmRegionListenerParams * /*params*/) {}

static void project_header_listener(const wmRegionListenerParams * /*params*/) {}

static void project_navigation_region_listener(const wmRegionListenerParams * /*params*/) {}

static void project_execute_region_listener(const wmRegionListenerParams * /*params*/) {}

static void project_space_blend_write(BlendWriter *writer, SpaceLink *sl)
{
  BLO_write_struct(writer, SpaceProject, sl);
}

void ED_spacetype_project()
{
  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();
  ARegionType *art;

  st->spaceid = SPACE_PROJECT;
  STRNCPY(st->name, "Project");

  st->create = project_create;
  st->free = project_free;
  st->init = project_init;
  st->duplicate = project_duplicate;
  st->operatortypes = project_operatortypes;
  st->keymap = project_keymap;
  st->blend_write = project_space_blend_write;

  /* regions: header */
  art = MEM_callocN<ARegionType>("spacetype project region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D | ED_KEYMAP_HEADER;
  art->init = project_header_region_init;
  art->draw = project_header_region_draw;
  art->listener = project_header_listener;

  BLI_addhead(&st->regiontypes, art);

  /* regions: main window */
  art = MEM_callocN<ARegionType>("spacetype project region");
  art->regionid = RGN_TYPE_WINDOW;
  art->init = project_main_region_init;
  /* art->layout = project_main_region_layout; */
  art->draw = ED_region_panels_draw;
  art->listener = project_main_region_listener;
  art->keymapflag = ED_KEYMAP_UI;

  BLI_addhead(&st->regiontypes, art);

  /* regions: navigation window */
  art = MEM_callocN<ARegionType>("spacetype project region");
  art->regionid = RGN_TYPE_NAV_BAR;
  art->prefsizex = UI_NAVIGATION_REGION_WIDTH;
  art->init = project_navigation_region_init;
  art->draw = project_navigation_region_draw;
  art->listener = project_navigation_region_listener;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_NAVBAR;

  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(std::move(st));
}
