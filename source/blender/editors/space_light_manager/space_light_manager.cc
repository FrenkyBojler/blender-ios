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
 #include "BKE_idprop.hh"
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
 #include "RNA_define.hh"

 #include "BLI_listbase.h"
 #include "BLI_string.h"

 #include "MEM_guardedalloc.h"

namespace blender::ed::light_manager {

struct SpaceLightManager_Runtime {
};

/* Inline helper to get SpaceLightManager from context. */
static inline SpaceLightManager *CTX_wm_space_light_manager(const bContext *C)
{
  ScrArea *area = CTX_wm_area(C);
  if (area && area->spacetype == SPACE_LIGHT_MANAGER) {
    return static_cast<SpaceLightManager *>(area->spacedata.first);
  }
  return nullptr;
}

/* Helper functions for group management */

static const char *get_light_group(Object *ob)
{
  if (ob->id.properties == nullptr) {
    return "Ungrouped";
  }
  
  IDProperty *prop = IDP_GetPropertyFromGroup(ob->id.properties, "light_mixer_group");
  if (prop && prop->type == IDP_STRING) {
    return IDP_string_get(prop);  /* Use macro instead of IDP_String */
  }
  return "Ungrouped";
}

static void set_light_group(Object *ob, const char *group_name)
{
  if (ob->id.properties == nullptr) {
    IDPropertyTemplate val = {0};
    ob->id.properties = IDP_New(IDP_GROUP, &val, "RNA_CustomProperties");
  }
  
  IDProperty *prop = IDP_GetPropertyFromGroup(ob->id.properties, "light_mixer_group");
  if (prop && prop->type == IDP_STRING) {
    IDP_FreeFromGroup(ob->id.properties, prop);
  }
  
  IDPropertyTemplate val;
  val.string.str = group_name;
  val.string.len = strlen(group_name) + 1;
  val.string.subtype = IDP_STRING_SUB_UTF8;
  prop = IDP_New(IDP_STRING, &val, "light_mixer_group");
  IDP_AddToGroup(ob->id.properties, prop);
}

static bool ED_operator_light_manager_active(bContext *C)
{
  return CTX_wm_space_light_manager(C) != nullptr;
}

/* Group Operators */

static wmOperatorStatus light_manager_group_add_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  if (!space_lm) {
    return OPERATOR_CANCELLED;
  }

  SpaceLightManagerGroup *group = MEM_callocN<SpaceLightManagerGroup>("LightManagerGroup");
  STRNCPY(group->name, "New Group");
  group->flag = 0;
  BLI_addtail(&space_lm->groups, group);

  /* Assign all selected lights to this new group. */
  CTX_DATA_BEGIN (C, Object *, ob, selected_objects) {
    if (ob->type == OB_LAMP) {
      set_light_group(ob, group->name);
    }
  }
  CTX_DATA_END;

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_LIGHT_MANAGER, space_lm);
  return OPERATOR_FINISHED;
}

static void LIGHT_MANAGER_OT_group_add(wmOperatorType *ot)
{
  ot->name = "Add Light Group";
  ot->description = "Add a new light group";
  ot->idname = "LIGHT_MANAGER_OT_group_add";
  ot->exec = light_manager_group_add_exec;
  ot->poll = ED_operator_light_manager_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus light_manager_group_delete_exec(bContext *C, wmOperator *op)
{
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  if (!space_lm) {
    return OPERATOR_CANCELLED;
  }

  const int index = RNA_int_get(op->ptr, "index");
  SpaceLightManagerGroup *group = static_cast<SpaceLightManagerGroup *>(
      BLI_findlink(&space_lm->groups, index));
  
  if (!group) {
    return OPERATOR_CANCELLED;
  }

  /* Reset lights that were in this group back to the default group name. */
  Scene *scene = CTX_data_scene(C);
  FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
    if (ob->type != OB_LAMP) {
      continue;
    }
    if (STREQ(get_light_group(ob), group->name)) {
      set_light_group(ob, "Ungrouped");
    }
  }
  FOREACH_SCENE_OBJECT_END;

  BLI_remlink(&space_lm->groups, group);
  MEM_freeN(group);
  
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_LIGHT_MANAGER, space_lm);
  return OPERATOR_FINISHED;
}

static void LIGHT_MANAGER_OT_group_delete(wmOperatorType *ot)
{
  ot->name = "Delete Light Group";
  ot->description = "Delete a light group";
  ot->idname = "LIGHT_MANAGER_OT_group_delete";
  ot->exec = light_manager_group_delete_exec;
  ot->poll = ED_operator_light_manager_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "", 0, INT_MAX);
}

static wmOperatorStatus light_manager_group_toggle_exec(bContext *C, wmOperator *op)
{
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  if (!space_lm) {
    return OPERATOR_CANCELLED;
  }

  const int index = RNA_int_get(op->ptr, "index");
  SpaceLightManagerGroup *group = static_cast<SpaceLightManagerGroup *>(
      BLI_findlink(&space_lm->groups, index));
  
  if (!group) {
    return OPERATOR_CANCELLED;
  }

  group->flag ^= SPACE_LIGHT_MANAGER_GROUP_COLLAPSED;
  
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_LIGHT_MANAGER, space_lm);
  return OPERATOR_FINISHED;
}

static void LIGHT_MANAGER_OT_group_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Group Visibility";
  ot->description = "Expand or collapse a light group";
  ot->idname = "LIGHT_MANAGER_OT_group_toggle";
  ot->exec = light_manager_group_toggle_exec;
  ot->poll = ED_operator_light_manager_active;
  ot->flag = OPTYPE_INTERNAL;
  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "", 0, INT_MAX);
}

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
  
  /* Add group button. */
  title_row.op("LIGHT_MANAGER_OT_group_add", "", ICON_ADD);

  layout.separator();

  /* Collect and sort all lights in the scene. */
  int light_count = 0;
  blender::Vector<Object *> lights;

  FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
    if (ob->type == OB_LAMP) {
      Light *light = static_cast<Light *>(ob->data);
      if (light != nullptr) {
        lights.append(ob);
        light_count++;
      }
    }
  }
  FOREACH_SCENE_OBJECT_END;

  /* Sort lights based on sort type stored in the space. */
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  eSpaceLightManagerSortType sort_type = LIGHT_MANAGER_SORT_NAME;
  if (space_lm != nullptr) {
    sort_type = static_cast<eSpaceLightManagerSortType>(space_lm->sort_type);
  }

  std::sort(lights.begin(), lights.end(), [sort_type](Object *a, Object *b) {
    switch (sort_type) {
      case LIGHT_MANAGER_SORT_TYPE: {
        /* Sort by light type, then by name. */
        Light *light_a = static_cast<Light *>(a->data);
        Light *light_b = static_cast<Light *>(b->data);
        if (light_a->type != light_b->type) {
          return light_a->type < light_b->type;
        }
        return BLI_strcasecmp(a->id.name + 2, b->id.name + 2) < 0;
      }
      case LIGHT_MANAGER_SORT_POWER: {
        /* Sort by energy (descending), then by name. */
        Light *light_a = static_cast<Light *>(a->data);
        Light *light_b = static_cast<Light *>(b->data);
        if (light_a->energy != light_b->energy) {
          return light_a->energy > light_b->energy;
        }
        return BLI_strcasecmp(a->id.name + 2, b->id.name + 2) < 0;
      }
      case LIGHT_MANAGER_SORT_NAME:
      default:
        /* Sort by object name. */
        return BLI_strcasecmp(a->id.name + 2, b->id.name + 2) < 0;
    }
  });

  /* Group lights by their assigned group. */
  blender::Map<std::string, blender::Vector<Object *>> grouped_lights;
  
  for (Object *ob : lights) {
    const char *group_name = get_light_group(ob);
    grouped_lights.lookup_or_add_default(group_name).append(ob);
  }

  /* Draw custom groups first. */
  if (space_lm && space_lm->groups.first) {
    LISTBASE_FOREACH (SpaceLightManagerGroup *, group, &space_lm->groups) {
      if (!grouped_lights.contains(group->name)) {
        continue;
      }

      layout.separator();
      ui::Layout &group_box = layout.box();

      /* Group header. */
      ui::Layout &group_header = group_box.row(false);

      /* Collapse/expand button. */
      int icon = (group->flag & SPACE_LIGHT_MANAGER_GROUP_COLLAPSED) ? 
                 ICON_DISCLOSURE_TRI_RIGHT : ICON_DISCLOSURE_TRI_DOWN;
      int group_index = BLI_findindex(&space_lm->groups, group);
      PointerRNA op_ptr = group_header.op("LIGHT_MANAGER_OT_group_toggle", "", icon);
      RNA_int_set(&op_ptr, "index", group_index);
      
      /* Group name. */
      group_header.label(group->name, ICON_NONE);
      
      /* Delete button. */
      PointerRNA del_op = group_header.op("LIGHT_MANAGER_OT_group_delete", "", ICON_X);
      RNA_int_set(&del_op, "index", group_index);
      layout.separator();

      /* Draw lights in this group if expanded. */
      if (!(group->flag & SPACE_LIGHT_MANAGER_GROUP_COLLAPSED)) {
        for (Object *ob : grouped_lights.lookup(group->name)) {
          Light *light = static_cast<Light *>(ob->data);
          
          PointerRNA ob_ptr = RNA_pointer_create_discrete(&scene->id, &RNA_Object, ob);
          PointerRNA light_ptr = RNA_pointer_get(&ob_ptr, "data");

          ui::Layout &light_row = group_box.row(false);
          light_row.use_property_split_set(false);
          light_row.use_property_decorate_set(false);

          if (ob->visibility_flag & OB_HIDE_VIEWPORT) {
            light_row.enabled_set(false);
          }

          /* Light type icon. */
          int type_icon = ICON_LIGHT;
          switch (light->type) {
            case LA_LOCAL: type_icon = ICON_LIGHT_POINT; break;
            case LA_SUN: type_icon = ICON_LIGHT_SUN; break;
            case LA_SPOT: type_icon = ICON_LIGHT_SPOT; break;
            case LA_AREA: type_icon = ICON_LIGHT_AREA; break;
          }
          light_row.label("", type_icon);

          /* Light properties. */
          ui::Layout &name_col = light_row.row(false);
          name_col.ui_units_x_set(12.0f);
          name_col.prop(&ob_ptr, "name", UI_ITEM_NONE, std::nullopt, ICON_NONE);

          ui::Layout &color_col = light_row.row(false);
          color_col.ui_units_x_set(3.0f);
          color_col.prop(&light_ptr, "color", UI_ITEM_NONE, std::nullopt, ICON_NONE);

          ui::Layout &intensity_col = light_row.row(false);
          intensity_col.ui_units_x_set(8.0f);
          intensity_col.prop(&light_ptr, "energy", UI_ITEM_NONE, std::nullopt, ICON_NONE);

          ui::Layout &vis_col = light_row.row(true);
          vis_col.ui_units_x_set(3.0f);
          vis_col.prop(&ob_ptr, "hide_viewport", UI_ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
          vis_col.prop(&ob_ptr, "hide_render", UI_ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
        }
      }
    }
  }
  
  /* Draw ungrouped lights. */
  if (grouped_lights.contains("Ungrouped") && !grouped_lights.lookup("Ungrouped").is_empty()) {
    layout.separator();
    ui::Layout &ungrouped_header = layout.row(false);
    ungrouped_header.label("=== Ungrouped ===", ICON_NONE);
    layout.separator();
    
    for (Object *ob : grouped_lights.lookup("Ungrouped")) {

      Light *light = static_cast<Light *>(ob->data);
      
      PointerRNA ob_ptr = RNA_pointer_create_discrete(&scene->id, &RNA_Object, ob);
      PointerRNA light_ptr = RNA_pointer_get(&ob_ptr, "data");;

      ui::Layout &light_row = layout.row(false);
      light_row.use_property_split_set(false);
      light_row.use_property_decorate_set(false);

      if (ob->visibility_flag & OB_HIDE_VIEWPORT) {
        light_row.enabled_set(false);
      }

      int type_icon = ICON_LIGHT;
      switch (light->type) {
        case LA_LOCAL: type_icon = ICON_LIGHT_POINT; break;
        case LA_SUN: type_icon = ICON_LIGHT_SUN; break;
        case LA_SPOT: type_icon = ICON_LIGHT_SPOT; break;
        case LA_AREA: type_icon = ICON_LIGHT_AREA; break;
      }
      light_row.label("", type_icon);

      ui::Layout &name_col = light_row.row(false);
      name_col.ui_units_x_set(12.0f);
      name_col.prop(&ob_ptr, "name", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      ui::Layout &color_col = light_row.row(false);
      color_col.ui_units_x_set(3.0f);
      color_col.prop(&light_ptr, "color", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      ui::Layout &intensity_col = light_row.row(false);
      intensity_col.ui_units_x_set(8.0f);
      intensity_col.prop(&light_ptr, "energy", UI_ITEM_NONE, std::nullopt, ICON_NONE);

      ui::Layout &vis_col = light_row.row(true);
      vis_col.ui_units_x_set(3.0f);
      vis_col.prop(&ob_ptr, "hide_viewport", UI_ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
      vis_col.prop(&ob_ptr, "hide_render", UI_ITEM_R_ICON_ONLY, std::nullopt, ICON_NONE);
    }
  }

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
  /* Use the standard Python-driven header implementation. */
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

static void light_manager_operatortypes()
{
  WM_operatortype_append(LIGHT_MANAGER_OT_group_add);
  WM_operatortype_append(LIGHT_MANAGER_OT_group_delete);
  WM_operatortype_append(LIGHT_MANAGER_OT_group_toggle);
}

static void light_manager_keymap(wmKeyConfig * /*keyconf*/)
{
}

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
  st->operatortypes = light_manager_operatortypes;
  st->keymap = light_manager_keymap;
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
