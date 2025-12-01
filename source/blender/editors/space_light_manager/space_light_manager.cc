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

#include "BLF_api.hh"

 #include "BKE_collection.hh"
 #include "BKE_context.hh"
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

 #include "DNA_light_types.h"
 #include "DNA_object_types.h"
 #include "DNA_space_types.h"

 #include "RNA_access.hh"

namespace blender::ed::light_manager {

struct SpaceLightManager_Runtime {
};

/* -------------------------------------------------------------------- */
/** \name Main Region
 * \{ */

static void light_manager_main_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  region->flag |= RGN_FLAG_INDICATE_OVERFLOW;
}

static void light_manager_main_region_draw(const bContext *C, ARegion *region)
{
  using namespace blender;
  
  UI_ThemeClearColor(TH_BACK);

  /* Get scene context. */
  Scene *scene = CTX_data_scene(C);

  /* Create UI block and layout. */
  uiBlock *block = UI_block_begin(C, region, __func__, ui::EmbossType::Emboss);
  
  ui::Layout &layout = ui::block_layout(block,
                                         ui::LayoutDirection::Vertical,
                                         ui::LayoutType::Panel,
                                         10,
                                         region->winy - 10,  /* Start from top */
                                         region->winx - 20,
                                         region->winy,
                                         0,
                                         UI_style_get());
  
  /* Title row. */
  ui::Layout &title_row = layout.row(false);
  title_row.label(IFACE_("Light Manager"), ICON_OUTLINER_OB_LIGHT);

  layout.separator();

  /* Iterate through all lights in the scene. */
  int light_count = 0;

  FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
    if (ob->type != OB_LAMP) {
      continue;
    }

    Light *light = static_cast<Light *>(ob->data);
    if (light == nullptr) {
      continue;
    }

    light_count++;

    /* Create PointerRNA for object and light data. */
    PointerRNA ob_ptr = RNA_pointer_create_discrete(&scene->id, &RNA_Object, ob);
    PointerRNA light_ptr = RNA_pointer_get(&ob_ptr, "data");

    /* Create a row for this light. */
    ui::Layout &light_row = layout.row(false);
    light_row.use_property_split_set(false);
    light_row.use_property_decorate_set(false);

    /* Disable (gray out) row if light is hidden in viewport. */
    if (ob->visibility_flag & OB_HIDE_VIEWPORT) {
      light_row.enabled_set(false);
    }

    /* Column 1: Light type icon (before name). */
    int type_icon = ICON_LIGHT;
    switch (light->type) {
      case LA_LOCAL:
        type_icon = ICON_LIGHT_POINT;
        break;
      case LA_SUN:
        type_icon = ICON_LIGHT_SUN;
        break;
      case LA_SPOT:
        type_icon = ICON_LIGHT_SPOT;
        break;
      case LA_AREA:
        type_icon = ICON_LIGHT_AREA;
        break;
    }
    light_row.label("", type_icon);

    /* Column 2: Light name (editable). */
    ui::Layout &name_col = light_row.row(false);
    name_col.ui_units_x_set(12.0f);
    name_col.prop(&ob_ptr, "name", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    /* Column 3: Light color. */
    ui::Layout &color_col = light_row.row(false);
    color_col.ui_units_x_set(3.0f);
    color_col.prop(&light_ptr, "color", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    /* Column 4: Intensity. */
    ui::Layout &intensity_col = light_row.row(false);
    intensity_col.ui_units_x_set(8.0f);
    intensity_col.prop(&light_ptr, "energy", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    /* Column 5-7: Visibility toggles. */
    ui::Layout &vis_col = light_row.row(true);  /* Aligned group */
    vis_col.ui_units_x_set(3.0f);
    
    /* Viewport visibility. */
    vis_col.prop(&ob_ptr, "hide_viewport", UI_ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
    
    /* Render visibility. */
    vis_col.prop(&ob_ptr, "hide_render", UI_ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
  }
  FOREACH_SCENE_OBJECT_END;

  /* If no lights found, show message. */
  if (light_count == 0) {
    layout.label(IFACE_("No lights in scene"), ICON_INFO);
  }

  ui::block_layout_resolve(block);
  UI_block_end(C, block);
  UI_block_draw(C, block);

  ED_region_draw_overflow_indication(CTX_wm_area(C), region);
}

static void light_manager_main_region_listener(const wmRegionListenerParams *params)
{
  ARegion *region = params->region;
  const wmNotifier *wmn = params->notifier;

  /* Context changes. */
  switch (wmn->category) {
    case NC_SCENE:
      /* Redraw when scene changes (lights added/removed). */
      switch (wmn->data) {
        case ND_OB_ACTIVE:
        case ND_OB_SELECT:
        case ND_OB_VISIBLE:
        case ND_LAYER_CONTENT:
          ED_region_tag_redraw(region);
          break;
      }
      break;
    case NC_OBJECT:
      /* Redraw when objects change (lights modified). */
      switch (wmn->data) {
        case ND_TRANSFORM:
        case ND_OB_SHADING:
        case ND_DRAW:
          ED_region_tag_redraw(region);
          break;
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_LIGHT_MANAGER) {
        ED_region_tag_redraw(region);
      }
      break;
  }
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
  uiBlock *block = UI_block_begin(C, region, __func__, ui::EmbossType::Emboss);
  
  /* Editor type selector (dropdown menu). */
  int xco = ED_area_header_switchbutton(C, block, 0);
  
  UI_block_end(C, block);
  UI_block_draw(C, block);
  
  /* Standard header menus. */
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
