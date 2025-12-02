/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup splightmanager
 */

 #include <cstring>

 #include "MEM_guardedalloc.h"

 #include "BLI_listbase.h"
 #include "BLI_map.hh"
 #include "BLI_string_utf8.h"
 #include "BLI_vector.hh"
 #include "BLI_rect.h"

 #include "BLT_translation.hh"

#include "BLF_api.hh"

 #include "GPU_matrix.hh"
 #include "GPU_state.hh"

 #include "BKE_collection.hh"
 #include "BKE_context.hh"
 #include "BKE_idprop.hh"
 #include "BKE_layer.hh"
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
 #include "UI_interface_c.hh"
 #include "../interface/interface_intern.hh"

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

static const char *light_manager_default_group_name()
{
  return "Scene light";
}

struct SubgroupCollapseState {
  SpaceLightManagerGroup *group;
  std::string name;
  bool collapsed;
};

struct GroupBounds {
  const char *group_name;
  int index;
  float y_min;  /* Bottom of group in view space */
  float y_max;  /* Top of group in view space */
};

struct SpaceLightManager_Runtime {
  blender::Vector<SubgroupCollapseState> subgroup_states;
  blender::Vector<GroupBounds> group_bounds;  /* Updated each draw */
};

static void light_manager_draw_drag_ghost(bContext *C, wmWindow *win, wmDrag *drag, const int xy[2]);

static void light_manager_ensure_default_group(SpaceLightManager *space_lm)
{
  if (space_lm == nullptr) {
    return;
  }
  if (space_lm->groups.first != nullptr) {
    return;
  }

  SpaceLightManagerGroup *group = MEM_callocN<SpaceLightManagerGroup>("LightManagerGroup");
  STRNCPY(group->name, light_manager_default_group_name());
  group->flag = 0;
  BLI_addtail(&space_lm->groups, group);
}

/* -------------------------------------------------------------------- */
/** \name Dedicated table-style layout for lights
 *  (pixel-based columns, similar spirit to Spreadsheet)
 * \{ */

enum class eLightManagerColumn {
  Drag = 0,
  Icon,
  Name,
  Color,
  Power,
  Visibility,
  Count,
};

struct LightManagerColumnLayout {
  const char *label;
  float width_fraction;
  int header_offset_x;
  int header_offset_y;
};

static const LightManagerColumnLayout light_manager_columns[] = {
    {"", 0.04f, 0, 0},          /* Drag handle (centered) */
    {"", 0.03f, 0, 0},          /* Light type icon (centered) */
    {"Name", 0.30f, 5, 0},      /* Object/group name (narrower) */
    {"Color", 0.25f, 5, 0},     /* Light color (slightly wider) */
    {"Power", 0.23f, 5, 0},     /* Light energy (slightly wider) */
    {"Visibility", 0.15f, 5, 0} /* Viewport/render toggles + remove (wider) */
};



/** \} */

/* -------------------------------------------------------------------- */
/** \name Table-based layout helpers for groups
 *  (pixel-precise columns aligned across all groups)
 * \{ */

struct ColumnLayout {
  int x;      /* Position X en pixels */
  int width;  /* Largeur en pixels */
};

/* Calculate pixel-precise column positions for the entire table. */
static void calculate_column_positions(int table_width,
                                       int x_start,
                                       ColumnLayout out_columns[int(eLightManagerColumn::Count)])
{
  for (int i = 0; i < int(eLightManagerColumn::Count); i++) {
    const float fraction = light_manager_columns[i].width_fraction;
    out_columns[i].width = int(float(table_width) * fraction);
    
    /* Calculate X position by summing widths of previous columns */
    out_columns[i].x = x_start;
    for (int j = 0; j < i; j++) {
      out_columns[i].x += out_columns[j].width;
    }
  }
}



/** \} */

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
    return nullptr;  /* No property = not in Light Manager */
  }
  
  IDProperty *prop = IDP_GetPropertyFromGroup(ob->id.properties, "light_mixer_group");
  if (prop && prop->type == IDP_STRING) {
    return IDP_string_get(prop);
  }
  return nullptr;  /* Property missing or wrong type = not in Light Manager */
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

static void remove_light_group(Object *ob)
{
  if (ob->id.properties == nullptr) {
    return;
  }
  
  IDProperty *prop = IDP_GetPropertyFromGroup(ob->id.properties, "light_mixer_group");
  if (prop) {
    IDP_FreeFromGroup(ob->id.properties, prop);
  }
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
  
  /* Find unique name with automatic numbering */
  char base_name[64] = "Group";
  int suffix = 1;
  bool name_is_unique = false;
  
  while (!name_is_unique) {
    char test_name[64];
    if (suffix == 1) {
      STRNCPY(test_name, base_name);
    } else {
      BLI_snprintf(test_name, sizeof(test_name), "%s.%03d", base_name, suffix);
    }
    
    /* Check if this name already exists */
    bool exists = false;
    LISTBASE_FOREACH (SpaceLightManagerGroup *, existing_group, &space_lm->groups) {
      if (STREQ(existing_group->name, test_name)) {
        exists = true;
        break;
      }
    }
    
    if (!exists) {
      STRNCPY(group->name, test_name);
      name_is_unique = true;
    } else {
      suffix++;
    }
  }
  
  group->flag = 0;
  BLI_addtail(&space_lm->groups, group);

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
    const char *light_group = get_light_group(ob);
    if (light_group && STREQ(light_group, group->name)) {
      /* Completely remove the light from Light Manager so this group name cannot
       * respawn later based on lingering properties. */
      remove_light_group(ob);
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

/* Assign selected lights to an existing group. */

static wmOperatorStatus light_manager_assign_selected_to_group_exec(bContext *C,
                                                                    wmOperator *op)
{
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  if (!space_lm) {
    return OPERATOR_CANCELLED;
  }

  /* Make sure there is at least one valid group so index 0 always refers
   * to something sensible even after deleting all groups. */


  const int index = RNA_int_get(op->ptr, "index");
  SpaceLightManagerGroup *group = static_cast<SpaceLightManagerGroup *>(
      BLI_findlink(&space_lm->groups, index));

  if (!group) {
    return OPERATOR_CANCELLED;
  }

  /* Move all selected light objects into this group. */
  CTX_DATA_BEGIN (C, Object *, ob, selected_objects) {
    if (ob->type == OB_LAMP) {
      set_light_group(ob, group->name);
    }
  }
  CTX_DATA_END;

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_LIGHT_MANAGER, space_lm);
  return OPERATOR_FINISHED;
}

static void LIGHT_MANAGER_OT_assign_selected_to_group(wmOperatorType *ot)
{
  ot->name = "Assign Selected Lights to Group";
  ot->description = "Move selected lights into this group";
  ot->idname = "LIGHT_MANAGER_OT_assign_selected_to_group";
  ot->exec = light_manager_assign_selected_to_group_exec;
  ot->poll = ED_operator_light_manager_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "", 0, INT_MAX);
}

/* Add a single light to a group via popup. */

static wmOperatorStatus light_manager_add_light_exec(bContext *C, wmOperator *op)
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

  /* Get the enum value (which is the light index in our list). */
  const int light_index = RNA_enum_get(op->ptr, "light_name");
  
  /* Find the light object by iterating through scene lights. */
  Scene *scene = CTX_data_scene(C);
  Object *ob = nullptr;
  int current_index = 0;
  
  FOREACH_SCENE_OBJECT_BEGIN (scene, ob_iter) {
    if (ob_iter->type == OB_LAMP) {
      if (current_index == light_index) {
        ob = ob_iter;
        break;
      }
      current_index++;
    }
  }
  FOREACH_SCENE_OBJECT_END;
  
  if (!ob) {
    return OPERATOR_CANCELLED;
  }

  set_light_group(ob, group->name);

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_LIGHT_MANAGER, space_lm);
  return OPERATOR_FINISHED;
}

/* Dynamic enum items callback to list all lights in the scene. */
static const EnumPropertyItem *light_manager_light_enum_itemf(bContext *C,
                                                               PointerRNA * /*ptr*/,
                                                               PropertyRNA * /*prop*/,
                                                               bool *r_free)
{
  static const EnumPropertyItem empty_items[] = {{0, nullptr, 0, nullptr, nullptr}};
  
  if (!C) {
    return empty_items;
  }

  Scene *scene = CTX_data_scene(C);
  if (!scene) {
    return empty_items;
  }

  EnumPropertyItem *items = nullptr;
  int totitem = 0;

  FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
    if (ob->type == OB_LAMP) {
      EnumPropertyItem tmp = {0};
      tmp.identifier = ob->id.name + 2;
      tmp.name = ob->id.name + 2;
      tmp.value = totitem;
      RNA_enum_item_add(&items, &totitem, &tmp);
    }
  }
  FOREACH_SCENE_OBJECT_END;

  RNA_enum_item_end(&items, &totitem);
  *r_free = true;

  return items;
}

static wmOperatorStatus light_manager_add_light_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent * /*event*/)
{
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  if (!space_lm) {
    return OPERATOR_CANCELLED;
  }

  /* Show dialog popup to select a light. */
  return WM_operator_props_dialog_popup(C, op, 300, IFACE_("Add Light to Group"));
}

static void LIGHT_MANAGER_OT_add_light(wmOperatorType *ot)
{
  static const EnumPropertyItem dummy_items[] = {{0, nullptr, 0, nullptr, nullptr}};
  
  ot->name = "Add Light to Group";
  ot->description = "Assign a chosen light from the scene to this group";
  ot->idname = "LIGHT_MANAGER_OT_add_light";
  ot->invoke = light_manager_add_light_invoke;
  ot->exec = light_manager_add_light_exec;
  ot->poll = ED_operator_light_manager_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Internal group index. */
  PropertyRNA *prop = RNA_def_property(ot->srna, "index", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  /* Light name as enum presented in the popup. */
  prop = RNA_def_enum(ot->srna, "light_name", dummy_items, 0, "Light", "Light to assign to the group");
  RNA_def_enum_funcs(prop, light_manager_light_enum_itemf);
}

/* Move groups up/down in the list. */

enum eLightManagerGroupMoveDirection {
  LIGHT_MANAGER_GROUP_MOVE_UP = 0,
  LIGHT_MANAGER_GROUP_MOVE_DOWN = 1,
};

/* Forward declarations */
static void LIGHT_MANAGER_OT_drop_light(wmOperatorType *ot);
static wmOperatorStatus group_drop_invoke(bContext *C, wmOperator *op, const wmEvent *event);

static wmOperatorStatus light_manager_group_move_exec(bContext *C, wmOperator *op)
{
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  if (!space_lm) {
    return OPERATOR_CANCELLED;
  }

  const int index = RNA_int_get(op->ptr, "index");
  const int direction = RNA_enum_get(op->ptr, "direction");

  SpaceLightManagerGroup *group = static_cast<SpaceLightManagerGroup *>(
      BLI_findlink(&space_lm->groups, index));
  if (!group) {
    return OPERATOR_CANCELLED;
  }

  if (direction == LIGHT_MANAGER_GROUP_MOVE_UP) {
    if (group->prev == nullptr) {
      return OPERATOR_CANCELLED;
    }
    BLI_remlink(&space_lm->groups, group);
    BLI_insertlinkbefore(&space_lm->groups, group->prev, group);
  }
  else if (direction == LIGHT_MANAGER_GROUP_MOVE_DOWN) {
    if (group->next == nullptr) {
      return OPERATOR_CANCELLED;
    }
    BLI_remlink(&space_lm->groups, group);
    BLI_insertlinkafter(&space_lm->groups, group->next, group);
  }

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_LIGHT_MANAGER, space_lm);
  return OPERATOR_FINISHED;
}

static void LIGHT_MANAGER_OT_group_move(wmOperatorType *ot)
{
  static const EnumPropertyItem move_dir_items[] = {
      {LIGHT_MANAGER_GROUP_MOVE_UP, "UP", ICON_TRIA_UP, "Up", "Move group up"},
      {LIGHT_MANAGER_GROUP_MOVE_DOWN, "DOWN", ICON_TRIA_DOWN, "Down", "Move group down"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Move Light Group";
  ot->description = "Reorder light groups";
  ot->idname = "LIGHT_MANAGER_OT_group_move";
  ot->exec = light_manager_group_move_exec;
  ot->invoke = group_drop_invoke;
  ot->poll = ED_operator_light_manager_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "", 0, INT_MAX);
  PropertyRNA *prop = RNA_def_property(ot->srna, "direction", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, move_dir_items);
}

/* Toggle visibility for all lights in a group. */

enum eLightManagerGroupVisibilityMode {
  LIGHT_MANAGER_GROUP_VISIBILITY_VIEWPORT = 0,
  LIGHT_MANAGER_GROUP_VISIBILITY_RENDER = 1,
};

static wmOperatorStatus light_manager_group_toggle_visibility_exec(bContext *C, wmOperator *op)
{
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  if (!space_lm) {
    return OPERATOR_CANCELLED;
  }

  const int index = RNA_int_get(op->ptr, "index");
  const int mode = RNA_enum_get(op->ptr, "mode");

  SpaceLightManagerGroup *group = static_cast<SpaceLightManagerGroup *>(
      BLI_findlink(&space_lm->groups, index));
  if (!group) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);

  /* Decide whether to hide or unhide: if any light in the group is visible,
   * hide all; otherwise unhide all. */
  bool any_viewport_visible = false;
  bool any_render_visible = false;

  FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
    if (ob->type != OB_LAMP) {
      continue;
    }
    const char *light_group = get_light_group(ob);
    if (light_group == nullptr || !STREQ(light_group, group->name)) {
      continue;
    }

    if (mode == LIGHT_MANAGER_GROUP_VISIBILITY_VIEWPORT &&
        (ob->visibility_flag & OB_HIDE_VIEWPORT) == 0)
    {
      any_viewport_visible = true;
    }
    if (mode == LIGHT_MANAGER_GROUP_VISIBILITY_RENDER &&
        (ob->visibility_flag & OB_HIDE_RENDER) == 0)
    {
      any_render_visible = true;
    }
  }
  FOREACH_SCENE_OBJECT_END;

  const bool new_viewport_hidden = (mode == LIGHT_MANAGER_GROUP_VISIBILITY_VIEWPORT) ?
                                       any_viewport_visible :
                                       false;
  const bool new_render_hidden = (mode == LIGHT_MANAGER_GROUP_VISIBILITY_RENDER) ?
                                      any_render_visible :
                                      false;

  FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
    if (ob->type != OB_LAMP) {
      continue;
    }
    const char *light_group = get_light_group(ob);
    if (light_group == nullptr || !STREQ(light_group, group->name)) {
      continue;
    }

    if (mode == LIGHT_MANAGER_GROUP_VISIBILITY_VIEWPORT) {
      if (new_viewport_hidden) {
        ob->visibility_flag |= OB_HIDE_VIEWPORT;
      }
      else {
        ob->visibility_flag &= ~OB_HIDE_VIEWPORT;
      }
    }
    else if (mode == LIGHT_MANAGER_GROUP_VISIBILITY_RENDER) {
      if (new_render_hidden) {
        ob->visibility_flag |= OB_HIDE_RENDER;
      }
      else {
        ob->visibility_flag &= ~OB_HIDE_RENDER;
      }
    }
  }
  FOREACH_SCENE_OBJECT_END;

  WM_event_add_notifier(C, NC_SCENE | ND_OB_VISIBLE, scene);
  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_LIGHT_MANAGER, space_lm);
  return OPERATOR_FINISHED;
}

static void LIGHT_MANAGER_OT_group_toggle_visibility(wmOperatorType *ot)
{
  static const EnumPropertyItem visibility_mode_items[] = {
      {LIGHT_MANAGER_GROUP_VISIBILITY_VIEWPORT,
       "VIEWPORT",
       ICON_RESTRICT_VIEW_OFF,
       "Viewport",
       "Toggle all lights in the group in the viewport"},
      {LIGHT_MANAGER_GROUP_VISIBILITY_RENDER,
       "RENDER",
       ICON_RESTRICT_RENDER_OFF,
       "Render",
       "Toggle all lights in the group for rendering"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Toggle Group Visibility";
  ot->description = "Toggle visibility of all lights in a group";
  ot->idname = "LIGHT_MANAGER_OT_group_toggle_visibility";
  ot->exec = light_manager_group_toggle_visibility_exec;
  ot->poll = ED_operator_light_manager_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna, "index", 0, 0, INT_MAX, "Index", "", 0, INT_MAX);
  PropertyRNA *prop = RNA_def_property(ot->srna, "mode", PROP_ENUM, PROP_NONE);
  RNA_def_property_enum_items(prop, visibility_mode_items);
}

/* Rename group. */

static wmOperatorStatus light_manager_group_rename_exec(bContext *C, wmOperator *op)
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

  /* Store old name so we can update lights that belong to this group. */
  char old_name[64];
  STRNCPY(old_name, group->name);

  char new_name[64];
  RNA_string_get(op->ptr, "name", new_name);
  if (new_name[0] != '\0') {
    /* Check if new name is already in use by another group */
    LISTBASE_FOREACH (SpaceLightManagerGroup *, other_group, &space_lm->groups) {
      if (other_group != group && STREQ(other_group->name, new_name)) {
        BKE_reportf(op->reports, RPT_ERROR, "Name '%s' already in use", new_name);
        return OPERATOR_CANCELLED;
      }
    }
    
    STRNCPY_UTF8(group->name, new_name);
  }

  /* Update all lights that referenced the old group name so they keep
   * belonging to this renamed group. */
  Scene *scene = CTX_data_scene(C);
  FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
    if (ob->type != OB_LAMP) {
      continue;
    }
    const char *light_group = get_light_group(ob);
    if (light_group && STREQ(light_group, old_name)) {
      set_light_group(ob, group->name);
    }
  }
  FOREACH_SCENE_OBJECT_END;

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_LIGHT_MANAGER, space_lm);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus light_manager_group_rename_invoke(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent * /*event*/)
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

  RNA_string_set(op->ptr, "name", group->name);
  /* Dialog popup with explicit OK/Cancel buttons. */
  return WM_operator_props_dialog_popup(C, op, 250, IFACE_("Rename Light Group"));
}

static void LIGHT_MANAGER_OT_group_rename(wmOperatorType *ot)
{
  ot->name = "Rename Light Group";
  ot->description = "Rename this light group";
  ot->idname = "LIGHT_MANAGER_OT_group_rename";
  ot->invoke = light_manager_group_rename_invoke;
  ot->exec = light_manager_group_rename_exec;
  ot->poll = ED_operator_light_manager_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* Internal index, hidden from the UI. */
  PropertyRNA *prop = RNA_def_property(ot->srna, "index", PROP_INT, PROP_NONE);
  RNA_def_property_range(prop, 0, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  /* Visible editable name field. */
  RNA_def_string(ot->srna, "name", nullptr, 64, "Name", "New group");
}

/* Remove a single light from its current group (send back to default group). */

static wmOperatorStatus light_manager_light_remove_from_group_exec(bContext *C, wmOperator *op)
{
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  if (!space_lm) {
    return OPERATOR_CANCELLED;
  }

  char name[MAX_ID_NAME];
  RNA_string_get(op->ptr, "object_name", name);
  if (name[0] == '\0') {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);
  Object *ob = nullptr;
  FOREACH_SCENE_OBJECT_BEGIN (scene, ob_iter) {
    if (STREQ(ob_iter->id.name + 2, name)) {
      ob = ob_iter;
      break;
    }
  }
  FOREACH_SCENE_OBJECT_END;

  if (!ob || ob->type != OB_LAMP) {
    return OPERATOR_CANCELLED;
  }

  /* Completely remove the light from Light Manager (remove the group property) */
  remove_light_group(ob);

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_LIGHT_MANAGER, space_lm);
  return OPERATOR_FINISHED;
}

static void LIGHT_MANAGER_OT_light_remove_from_group(wmOperatorType *ot)
{
  ot->name = "Remove Light From Group";
  ot->description = "Remove this light from its current group";
  ot->idname = "LIGHT_MANAGER_OT_light_remove_from_group";
  ot->exec = light_manager_light_remove_from_group_exec;
  ot->poll = ED_operator_light_manager_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(ot->srna,
                 "object_name",
                 nullptr,
                 MAX_ID_NAME,
                 "Light",
                 "Name of the light to unassign from the group");
}

/* -------------------------------------------------------------------- */
/** \name Main Region
 * \{ */

static void light_manager_main_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  region->flag |= RGN_FLAG_INDICATE_OVERFLOW;
  
  /* Add dropbox handler for drag and drop functionality. */
  ListBase *lb = WM_dropboxmap_find("Light Manager", SPACE_LIGHT_MANAGER, RGN_TYPE_WINDOW);
  WM_event_add_dropbox_handler(&region->runtime->handlers, lb);
}

static void light_manager_main_region_draw(const bContext *C, ARegion *region)
{
  using namespace blender;
  
  UI_ThemeClearColor(TH_BACK);

  Scene *scene = CTX_data_scene(C);
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);

  if (space_lm && !space_lm->runtime) {
    space_lm->runtime = MEM_new<SpaceLightManager_Runtime>(__func__);
  }
  if (space_lm && space_lm->runtime) {
    space_lm->runtime->group_bounds.clear();
  }

  auto draw_row_bg = [&](int y_top, int height, const float color[4]) {
    const float pad = 0.75f; /* uniform padding around each row background */

    rctf rect;
    rect.xmin = pad;
    rect.xmax = (float)region->winx - pad;
    rect.ymax = (float)y_top - pad;
    rect.ymin = (float)(y_top - height) + pad;
    
    float final_col[4];
    final_col[0] = color[0];
    final_col[1] = color[1];
    final_col[2] = color[2];
    final_col[3] = color[3];

    /* Filled rectangle for background */
    UI_draw_roundbox_4fv(&rect, true, 0.0f, final_col);
  };

  uiBlock *block = UI_block_begin(C, region, __func__, ui::EmbossType::Emboss);
  
  /* Constants */
  const int row_height = int(UI_UNIT_Y * 1.2f);
  const int top_bar_height = int(UI_UNIT_Y * 1.5f);
  const int margin = 10;
  const int group_vertical_padding = int(UI_UNIT_Y * 0.5f);

  /* Shared colors for group header and zebra rows (exactly two base shades). */
  float group_row_color[4];
  float zebra_color_even[4];
  float zebra_color_odd[4];
  UI_GetThemeColor4fv(TH_HEADER, group_row_color);
  for (int i = 0; i < 4; i++) {
    zebra_color_even[i] = group_row_color[i];
  }
  UI_GetThemeColorShade4fv(TH_HEADER, -10, zebra_color_odd);
  
  /* Start Y at top of region */
  int y = region->winy - margin;

  /* --- Top Bar (Manual Draw) --- */
  {
    uiDefIconTextButO(block,
                      ButType::But,
                      "LIGHT_MANAGER_OT_group_add",
                      wm::OpCallContext::InvokeDefault,
                      ICON_ADD,
                      "Add a new group",
                      margin,
                      y - top_bar_height,
                      200,
                      short(top_bar_height),
                      std::nullopt);
  }
  
  y -= (top_bar_height + margin);

  /* --- Collect Lights --- */
  blender::Vector<Object *> lights;
  FOREACH_SCENE_OBJECT_BEGIN (scene, ob) {
    if (ob->type == OB_LAMP) {
      lights.append(ob);
    }
  }
  FOREACH_SCENE_OBJECT_END;
  
  /* Sort lights */
  eSpaceLightManagerSortType sort_type = space_lm ? static_cast<eSpaceLightManagerSortType>(space_lm->sort_type) : LIGHT_MANAGER_SORT_NAME;
  std::sort(lights.begin(), lights.end(), [sort_type](Object *a, Object *b) {
    switch (sort_type) {
      case LIGHT_MANAGER_SORT_POWER: {
        Light *light_a = static_cast<Light *>(a->data);
        Light *light_b = static_cast<Light *>(b->data);
        if (light_a->energy != light_b->energy) return light_a->energy > light_b->energy;
        return BLI_strcasecmp(a->id.name + 2, b->id.name + 2) < 0;
      }
      case LIGHT_MANAGER_SORT_TYPE: {
        Light *light_a = static_cast<Light *>(a->data);
        Light *light_b = static_cast<Light *>(b->data);
        if (light_a->type != light_b->type) return light_a->type < light_b->type;
        return BLI_strcasecmp(a->id.name + 2, b->id.name + 2) < 0;
      }
      case LIGHT_MANAGER_SORT_NAME:
      default:
        return BLI_strcasecmp(a->id.name + 2, b->id.name + 2) < 0;
    }
  });
  
  /* Group lights */
  blender::Map<std::string, blender::Vector<Object *>> grouped_lights;
  for (Object *ob : lights) {
    const char *group_name = get_light_group(ob);
    if (group_name) {
      grouped_lights.lookup_or_add_default(group_name).append(ob);
    }
  }

  /* --- Table Setup --- */
  const int table_width = region->winx - 2 * margin;
  ColumnLayout columns[int(eLightManagerColumn::Count)];
  calculate_column_positions(table_width, margin, columns);

  /* --- Header --- */
  {
    for (int col = 0; col < int(eLightManagerColumn::Count); col++) {
        const char *label = light_manager_columns[col].label;
        if (label[0] == '\0') continue;
        
        const int offset_x = light_manager_columns[col].header_offset_x;
        const int offset_y = light_manager_columns[col].header_offset_y;
        
        uiDefBut(block, ButType::Label, label,
                 columns[col].x + offset_x, y - row_height + offset_y,
                 short(columns[col].width - offset_x), short(row_height),
                 nullptr, 0.0f, 0.0f, std::nullopt);
    }
    y -= row_height;
  }
  
  /* --- Groups Loop --- */
  if (space_lm && space_lm->groups.first) {
    LISTBASE_FOREACH (SpaceLightManagerGroup *, group, &space_lm->groups) {
      /* Use a safe, null-terminated copy of the group name when looking up in the map, to
       * avoid issues with corrupted or legacy data causing bad C-strings. */
      char group_name_safe[sizeof(group->name)];
      BLI_strncpy(group_name_safe, group->name, sizeof(group_name_safe));
      std::string group_key(group_name_safe);

      blender::Vector<Object *> *group_lights_ptr = grouped_lights.lookup_ptr(group_key);
      
      /* Calculate bounds for Drag & Drop */
      float group_start_y = (float)y;
      float group_h = (float)row_height;
      if (!(group->flag & SPACE_LIGHT_MANAGER_GROUP_COLLAPSED)) {
        int visible_rows = 0;
        if (group_lights_ptr && group_lights_ptr->size() != 0) {
          visible_rows = group_lights_ptr->size();
        }
        else {
          /* Show one informational row for empty expanded groups. */
          visible_rows = 1;
        }
        group_h += (float)row_height * visible_rows;
      }
      group_h += (float)group_vertical_padding;
      
      if (space_lm->runtime) {
        GroupBounds bounds;
        bounds.group_name = group->name;
        bounds.index = BLI_findindex(&space_lm->groups, group);
        bounds.y_max = group_start_y;
        bounds.y_min = group_start_y - group_h;
        space_lm->runtime->group_bounds.append(bounds);
      }

      /* Group visibility summary for header icons. */
      bool any_viewport_visible = false;
      bool any_viewport_hidden = false;
      bool any_render_visible = false;
      bool any_render_hidden = false;
      if (group_lights_ptr != nullptr) {
        for (Object *ob_vis : *group_lights_ptr) {
          if (ob_vis->type != OB_LAMP) {
            continue;
          }
          if ((ob_vis->visibility_flag & OB_HIDE_VIEWPORT) == 0) {
            any_viewport_visible = true;
          }
          else {
            any_viewport_hidden = true;
          }
          if ((ob_vis->visibility_flag & OB_HIDE_RENDER) == 0) {
            any_render_visible = true;
          }
          else {
            any_render_hidden = true;
          }
        }
      }

      /* Group Row Background */
      {
          draw_row_bg(y, row_height, group_row_color);
      }

      /* Group Row */
      {
        /* Drag Handle (Column 0) - used for group reordering via WM_DRAG_NAME. */
        {
             const int col = int(eLightManagerColumn::Drag);
             uiBut *drag_group_but = uiDefIconBut(block,
                                                  ButType::Label,
                                                  ICON_GRIP,
                                                  columns[col].x,
                                                  y - row_height,
                                                  short(columns[col].width),
                                                  short(row_height),
                                                  nullptr,
                                                  0.0f,
                                                  0.0f,
                                                  std::nullopt);
             if (drag_group_but != nullptr) {
               UI_but_drag_set_name(drag_group_but, group->name);
             }
        }

        /* Icon Column (Column 1) - Used for Collapse (centered in column, wider hit area). */
        {
            const int col = int(eLightManagerColumn::Icon);
            const int icon_glyph_w = UI_UNIT_X;
            const int pad_x = int(15 * UI_SCALE_FAC); /* 15 px padding on each side. */
            int button_w = icon_glyph_w + 2 * pad_x;
            if (button_w > columns[col].width) {
              button_w = columns[col].width;
            }
            const int button_x = columns[col].x + ((columns[col].width - button_w) / 2);
            int icon = (group->flag & SPACE_LIGHT_MANAGER_GROUP_COLLAPSED) ? ICON_DISCLOSURE_TRI_RIGHT : ICON_DISCLOSURE_TRI_DOWN;
            
             uiBut *but = uiDefIconButO(block,
                           ButType::But,
                           "LIGHT_MANAGER_OT_group_toggle",
                           wm::OpCallContext::InvokeDefault,
                           icon,
                           button_x,
                           y - row_height,
                           short(button_w),
                           short(row_height),
                           std::nullopt);
             if (but) {
                 UI_but_operator_ptr_ensure(but);
                 RNA_int_set(but->opptr, "index", BLI_findindex(&space_lm->groups, group));
             }
        }

        /* Name Column (Column 2) */
        {
            const int col = int(eLightManagerColumn::Name);
            /* Group Name (Rename Operator) - keep within the Name column and add small padding. */
            const int padding_x = 8;
            const int x = columns[col].x + padding_x;
            const short w = short(columns[col].width - padding_x);

            char group_name_safe[sizeof(group->name)];
            BLI_strncpy(group_name_safe, group->name, sizeof(group_name_safe));

            uiBut *but = uiDefButO(block,
                           ButType::But,
                           "LIGHT_MANAGER_OT_group_rename",
                           wm::OpCallContext::InvokeDefault,
                           group_name_safe,
                           x,
                           y - row_height,
                           w,
                           short(row_height),
                           std::nullopt);
            if (but) {
                UI_but_operator_ptr_ensure(but);
                RNA_int_set(but->opptr, "index", BLI_findindex(&space_lm->groups, group));
                UI_but_drawflag_enable(but, UI_BUT_TEXT_LEFT);
            }
        }
        
        /* Visibility Column (Column 5) - Group Visibility */
        {
            const int col = int(eLightManagerColumn::Visibility);
            const int icon_w = UI_UNIT_X;
            /* Center three icons in the visibility column. */
            int x = columns[col].x + ((columns[col].width - (icon_w * 3)) / 2);

            /* Choose icons based on aggregated group visibility state. */
            int icon_view = ICON_RESTRICT_VIEW_OFF;   /* eye open (visible) */
            if (!any_viewport_visible && any_viewport_hidden) {
              /* All lights hidden in viewport. */
              icon_view = ICON_RESTRICT_VIEW_ON;
            }

            int icon_render = ICON_RESTRICT_RENDER_OFF;  /* render allowed */
            if (!any_render_visible && any_render_hidden) {
              /* All lights disabled for render. */
              icon_render = ICON_RESTRICT_RENDER_ON;
            }

            /* Viewport */
             uiBut *but_v = uiDefIconButO(block,
                           ButType::But,
                           "LIGHT_MANAGER_OT_group_toggle_visibility",
                           wm::OpCallContext::InvokeDefault,
                           icon_view,
                           x,
                           y - row_height,
                           short(icon_w),
                           short(row_height),
                           std::nullopt);
             if (but_v) {
                 UI_but_operator_ptr_ensure(but_v);
                 RNA_int_set(but_v->opptr, "index", BLI_findindex(&space_lm->groups, group));
                 RNA_enum_set(but_v->opptr, "mode", LIGHT_MANAGER_GROUP_VISIBILITY_VIEWPORT);
             }
             x += icon_w;

            /* Render */
             uiBut *but_r = uiDefIconButO(block,
                           ButType::But,
                           "LIGHT_MANAGER_OT_group_toggle_visibility",
                           wm::OpCallContext::InvokeDefault,
                           icon_render,
                           x,
                           y - row_height,
                           short(icon_w),
                           short(row_height),
                           std::nullopt);
             if (but_r) {
                 UI_but_operator_ptr_ensure(but_r);
                 RNA_int_set(but_r->opptr, "index", BLI_findindex(&space_lm->groups, group));
                 RNA_enum_set(but_r->opptr, "mode", LIGHT_MANAGER_GROUP_VISIBILITY_RENDER);
             }
             x += icon_w;
             
             /* Delete Group */
             uiBut *but_d = uiDefIconButO(block, ButType::But, "LIGHT_MANAGER_OT_group_delete",
                           wm::OpCallContext::InvokeDefault, ICON_X,
                           x, y - row_height, short(icon_w), short(row_height), std::nullopt);
             if (but_d) {
                 UI_but_operator_ptr_ensure(but_d);
                 RNA_int_set(but_d->opptr, "index", BLI_findindex(&space_lm->groups, group));
             }
        }
      }
      y -= row_height;
      
      /* Lights Rows / Empty Group Message */
      if (!(group->flag & SPACE_LIGHT_MANAGER_GROUP_COLLAPSED)) {
        if (group_lights_ptr && group_lights_ptr->size() != 0) {
          int light_idx = 0;
          for (Object *ob : *group_lights_ptr) {
            /* Zebra striping for lights: exactly two base colors. Start with darker row. */
            {
              const float *col = (light_idx % 2 == 0) ? zebra_color_odd : zebra_color_even;
              draw_row_bg(y, row_height, col);
            }
            light_idx++;

            PointerRNA ob_ptr = RNA_pointer_create_discrete(&scene->id, &RNA_Object, ob);
            PointerRNA light_ptr = RNA_pointer_get(&ob_ptr, "data");
            
            /* Drag (Col 0) */
            {
                const int col = int(eLightManagerColumn::Drag);
                uiBut *drag_but = uiDefIconBut(block, ButType::Label, ICON_GRIP,
                                             columns[col].x + 10, y - row_height, short(columns[col].width - 10), short(row_height),
                                             nullptr, 0.0f, 0.0f, std::nullopt);
                 UI_but_drag_set_id(drag_but, &ob->id);
            }
            
            /* Icon (Col 1) - Light Type (centered to match group toggle). */
            {
                const int col = int(eLightManagerColumn::Icon);
                const int icon_w = UI_UNIT_X;
                const int icon_x = columns[col].x + ((columns[col].width - icon_w) / 2);
                Light *light = static_cast<Light *>(ob->data);
                int type_icon = ICON_LIGHT;
                switch (light->type) {
                    case LA_LOCAL: type_icon = ICON_LIGHT_POINT; break;
                    case LA_SUN: type_icon = ICON_LIGHT_SUN; break;
                    case LA_SPOT: type_icon = ICON_LIGHT_SPOT; break;
                    case LA_AREA: type_icon = ICON_LIGHT_AREA; break;
                }
                uiDefIconBut(block,
                             ButType::Label,
                             type_icon,
                             icon_x,
                             y - row_height,
                             short(icon_w),
                             short(row_height),
                             nullptr,
                             0.0f,
                             0.0f,
                             std::nullopt);
            }
            
            /* Name (Col 2) - Indented, fills the Name column up to Color. */
            {
                const int col = int(eLightManagerColumn::Name);
                PropertyRNA *prop = RNA_struct_find_property(&ob_ptr, "name");
                if (prop) {
                    const int indent = 20;
                    const int x = columns[col].x + indent;
                    const short w = short(columns[col].width - indent);
                    uiDefButR_prop(block,
                                   ButType::Text,
                                   "",
                                   x,
                                   y - row_height,
                                   w,
                                   short(row_height),
                                   &ob_ptr,
                                   prop,
                                   -1,
                                   0.0f,
                                   0.0f,
                                   std::nullopt);
                }
            }
            
            /* Color (Col 3) */
            {
                const int col = int(eLightManagerColumn::Color);
                PropertyRNA *prop = RNA_struct_find_property(&light_ptr, "color");
                if (prop) {
                    uiDefButR_prop(block, ButType::Color, std::nullopt,
                                   columns[col].x, y - row_height, short(columns[col].width), short(row_height),
                                   &light_ptr, prop, -1, 0.0f, 0.0f, std::nullopt);
                }
            }
            
            /* Power (Col 4) - full-width slider, center its text/value. */
            {
                const int col = int(eLightManagerColumn::Power);
                PropertyRNA *prop = RNA_struct_find_property(&light_ptr, "energy");
                if (prop) {
                    uiBut *but_power = uiDefButR_prop(block,
                                   ButType::NumSlider,
                                   std::nullopt,
                                   columns[col].x,
                                   y - row_height,
                                   short(columns[col].width),
                                   short(row_height),
                                   &light_ptr,
                                   prop,
                                   -1,
                                   0.0f,
                                   0.0f,
                                   std::nullopt);
                    if (but_power != nullptr) {
                        /* Enlever les flags gauche/droite pour obtenir un texte centré. */
                        UI_but_drawflag_disable(but_power, UI_BUT_TEXT_LEFT);
                        UI_but_drawflag_disable(but_power, UI_BUT_TEXT_RIGHT);
                    }
                }
            }
            
            /* Visibility (Col 5) */
            {
                const int col = int(eLightManagerColumn::Visibility);
                const int icon_w = UI_UNIT_X;
                /* Center three icons in the visibility column, matching group header. */
                int x = columns[col].x + ((columns[col].width - (icon_w * 3)) / 2);
                
                PropertyRNA *prop_hide_view = RNA_struct_find_property(&ob_ptr, "hide_viewport");
                if (prop_hide_view) {
                    uiDefButR_prop(block, ButType::IconToggle, std::nullopt,
                                   x, y - row_height, short(icon_w), short(row_height),
                                   &ob_ptr, prop_hide_view, -1, 0.0f, 0.0f, std::nullopt);
                }
                x += icon_w;
                
                PropertyRNA *prop_hide_render = RNA_struct_find_property(&ob_ptr, "hide_render");
                if (prop_hide_render) {
                    uiDefButR_prop(block, ButType::IconToggle, std::nullopt,
                                   x, y - row_height, short(icon_w), short(row_height),
                                   &ob_ptr, prop_hide_render, -1, 0.0f, 0.0f, std::nullopt);
                }
                x += icon_w;
                
                /* Remove button */
                uiBut *remove_but = uiDefIconButO(block, ButType::But,
                                                  "LIGHT_MANAGER_OT_light_remove_from_group",
                                                  blender::wm::OpCallContext::InvokeDefault,
                                                  ICON_X, x, y - row_height,
                                                  short(icon_w), short(row_height),
                                                  std::nullopt);
                if (remove_but != nullptr) {
                    UI_but_operator_ptr_ensure(remove_but);
                    RNA_string_set(remove_but->opptr, "object_name", ob->id.name + 2);
                }
            }
            y -= row_height;
          }
        }
        else {
          /* Expanded group with no lights: show an informational row, using the darker
           * zebra color so it matches the first light row background. */
          const int name_col = int(eLightManagerColumn::Name);
          const int indent = 20;
          const int msg_x = columns[name_col].x + indent;
          const short msg_w = short(table_width - (msg_x - margin));

          draw_row_bg(y, row_height, zebra_color_odd);

          uiDefBut(block,
                   ButType::Label,
                   IFACE_("No lights in this group"),
                   msg_x,
                   y - row_height,
                   msg_w,
                   short(row_height),
                   nullptr,
                   0.0f,
                   0.0f,
                   std::nullopt);

          y -= row_height;
        }
      }
      y -= group_vertical_padding;
    }
  }
  
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
      /* Redraw when objects or their data change (lights modified, type changed, etc.). */
      switch (wmn->data) {
        case ND_TRANSFORM:
        case ND_OB_SHADING:
        case ND_DRAW:
        case ND_DATA:       /* Light data (type, color, energy, etc.) changed. */
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
  light_manager_ensure_default_group(space_lm);

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

  /* Sanitize group names to be null-terminated even for legacy/corrupt files. */
  LISTBASE_FOREACH (SpaceLightManagerGroup *, group, &space_lm->groups) {
    group->name[sizeof(group->name) - 1] = '\0';
  }
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
  WM_operatortype_append(LIGHT_MANAGER_OT_assign_selected_to_group);
  WM_operatortype_append(LIGHT_MANAGER_OT_add_light);
  WM_operatortype_append(LIGHT_MANAGER_OT_group_move);
  WM_operatortype_append(LIGHT_MANAGER_OT_group_toggle_visibility);
  WM_operatortype_append(LIGHT_MANAGER_OT_group_rename);
  WM_operatortype_append(LIGHT_MANAGER_OT_light_remove_from_group);
  WM_operatortype_append(LIGHT_MANAGER_OT_drop_light);
}

/* Draw a simple ghost row in the Light Manager while dragging lights or groups. */
static void light_manager_draw_drag_ghost(bContext *C, wmWindow * /*win*/, wmDrag *drag, const int xy[2])
{
  /* Only draw for light or group drags used by Light Manager. */
  if (!(drag->type == WM_DRAG_ID || drag->type == WM_DRAG_NAME)) {
    return;
  }

  ARegion *region = CTX_wm_region(C);
  if (region == nullptr) {
    return;
  }

  /* Convert window coordinates to region local pixel-space. */
  int mx = xy[0] - region->winrct.xmin;
  int my = xy[1] - region->winrct.ymin;

  const int row_height = int(UI_UNIT_Y * 1.2f);
  const float pad = 1.0f;

  rctf rect;
  rect.xmin = pad;
  rect.xmax = float(region->winx) - pad;
  rect.ymax = float(my) + float(row_height) * 0.5f;
  rect.ymin = rect.ymax - float(row_height);

  bThemeState theme_state;
  UI_Theme_Store(&theme_state);
  UI_SetTheme(SPACE_LIGHT_MANAGER, RGN_TYPE_WINDOW);

  GPU_matrix_push();
  wmOrtho2_region_pixelspace(region);
  GPU_blend(GPU_BLEND_ALPHA_PREMULT);

  float col_bg[4];
  UI_GetThemeColor4fv(TH_BACK, col_bg);
  /* Darken strongly to get an almost-black ghost. */
  col_bg[0] *= 0.0f;
  col_bg[1] *= 0.0f;
  col_bg[2] *= 0.0f;
  col_bg[3] = 0.35f; /* fairly opaque but still a preview */
  UI_draw_roundbox_4fv(&rect, true, 0.0f, col_bg);

  GPU_blend(GPU_BLEND_NONE);
  GPU_matrix_pop();
  UI_Theme_Restore(&theme_state);
}

/* -------------------------------------------------------------------- */
/** \name Drop Boxes
 * \{ */

/* Check if drag data can be dropped - light objects only. */
static bool light_drop_poll(bContext * /*C*/, wmDrag *drag, const wmEvent * /*event*/)
{

  
  /* Only accept light objects. */
  if (drag->type == WM_DRAG_ID) {
    ID *id = WM_drag_get_local_ID(drag, ID_OB);
  
    if (id) {
      Object *ob = reinterpret_cast<Object *>(id);
    
      bool result = (ob->type == OB_LAMP);
    
      return result;
    }
  }

  return false;
}

/* Helper function to find which group is at a given Y coordinate. */
static int find_group_at_position(SpaceLightManager *space_lm, ARegion *region, const wmEvent *event)
{
  if (!space_lm || !region || !space_lm->runtime) {
    return 0;
  }

  /* Use cached bounds from draw */
  int mouse_y = event->mval[1];
  
  for (const GroupBounds &bounds : space_lm->runtime->group_bounds) {
    if (mouse_y <= bounds.y_max && mouse_y >= bounds.y_min) {
      return bounds.index;
    }
  }

  /* Default to last group if not found */
  return BLI_listbase_count(&space_lm->groups) - 1;
}

/* Dedicated operator for dropping lights (no popup). */
static wmOperatorStatus light_manager_drop_light_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  ID *id = WM_drag_get_local_ID_from_event(event, ID_OB);

  if (!id || GS(id->name) != ID_OB) {
  
    return OPERATOR_CANCELLED;
  }

  Object *ob = reinterpret_cast<Object *>(id);
  if (ob->type != OB_LAMP) {
  
    return OPERATOR_CANCELLED;
  }
  


  /* Detect which group the mouse is over. */
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  if (!space_lm) {
  
    return OPERATOR_CANCELLED;
  }
  
  ARegion *region = CTX_wm_region(C);
  int target_group_index = 0;  /* Default to first group */
  
  if (space_lm && region) {
    target_group_index = find_group_at_position(space_lm, region, event);
  }



  SpaceLightManagerGroup *group = static_cast<SpaceLightManagerGroup *>(
      BLI_findlink(&space_lm->groups, target_group_index));
  if (!group) {
  
    return OPERATOR_CANCELLED;
  }
  
  /* Assign lights to the group using the object's light group property.
   * - The dragged light is ALWAYS assigned.
   * - Additionally, all selected lamps are assigned too.
   * This way, dragging one light with others selected moves the whole selection,
   * but we never ignore the dragged light itself. */
 
  /* Distinguish between multi-ID drags from the Outliner and single-ID drags from the
   * Light Manager. Outliner drags may populate wmDrag::ids with multiple objects, while
   * the Light Manager grip only creates a single-ID drag. Use the drag list to decide
   * whether to move multiple lamps or just the dragged one. */

  bool handled_multi_drag = false;

  if (event->custom == EVT_DATA_DRAGDROP && event->customdata != nullptr) {
    ListBase *lb = static_cast<ListBase *>(event->customdata);
    wmDrag *drag = static_cast<wmDrag *>(lb->first);

    if (drag && drag->type == WM_DRAG_ID) {
      wmDragID *first_id = static_cast<wmDragID *>(drag->ids.first);
      wmDragID *second_id = first_id ? first_id->next : nullptr;

      /* When there is more than one ID in the drag, assume this comes from the Outliner,
       * which deliberately packs the current selection into the drag. Move all lamp IDs. */
      if (second_id != nullptr) {
        LISTBASE_FOREACH (wmDragID *, drag_id, &drag->ids) {
          if (!drag_id->id || GS(drag_id->id->name) != ID_OB) {
            continue;
          }

          Object *drag_ob = reinterpret_cast<Object *>(drag_id->id);
          if (drag_ob->type != OB_LAMP) {
            continue;
          }

          set_light_group(drag_ob, group->name);
        }

        handled_multi_drag = true;
      }
    }
  }

  /* Single-ID drag: Light Manager grip, or Outliner with only one object in the drag.
   * In this case, move only the dragged light, regardless of Outliner selection state. */
  if (!handled_multi_drag) {
    set_light_group(ob, group->name);
  }

  WM_event_add_notifier(C, NC_SPACE | ND_SPACE_LIGHT_MANAGER, nullptr);
  

  return OPERATOR_FINISHED;
}

static void LIGHT_MANAGER_OT_drop_light(wmOperatorType *ot)
{
  ot->name = "Drop Light";
  ot->description = "Drop a light into a group";
  ot->idname = "LIGHT_MANAGER_OT_drop_light";
  ot->invoke = light_manager_drop_light_invoke;
  ot->poll = ED_operator_light_manager_active;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;
}

/* -------------------------------------------------------------------- */
/** \name Group Reordering Drop Handlers
 * \{ */

/* Check if drag data is a group name for reordering. */
static bool group_drop_poll(bContext * /*C*/, wmDrag *drag, const wmEvent * /*event*/)
{

  bool result = (drag->type == WM_DRAG_NAME);

  return result;
}

/* Handle dropping a group to reorder it. */
static wmOperatorStatus group_drop_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{

  
  if (event->custom != EVT_DATA_DRAGDROP) {
  
    return OPERATOR_CANCELLED;
  }

  ListBase *lb = static_cast<ListBase *>(event->customdata);
  wmDrag *drag = static_cast<wmDrag *>(lb->first);
  
  if (drag->type != WM_DRAG_NAME || !drag->poin) {
    return OPERATOR_CANCELLED;
  }

  /* Get the dragged group name. */
  const char *dragged_group_name = static_cast<const char *>(drag->poin);
  
  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);
  if (!space_lm) {
    return OPERATOR_CANCELLED;
  }

  /* Find the source group index. */
  int source_index = -1;
  int current_index = 0;
  LISTBASE_FOREACH (SpaceLightManagerGroup *, group, &space_lm->groups) {
    if (STREQ(group->name, dragged_group_name)) {
      source_index = current_index;
      break;
    }
    current_index++;
  }

  if (source_index == -1) {
    return OPERATOR_CANCELLED;
  }
  
  int total_groups = BLI_listbase_count(&space_lm->groups);
  
  /* For now, just move one position down (simpler, no freeze risk).
   * User can drop multiple times to move further.
   * TODO: Detect exact target position based on mouse Y. */
  if (source_index >= total_groups - 1) {
  
    return OPERATOR_CANCELLED;
  }

  /* Set up the move operator to move down one position. */
  RNA_int_set(op->ptr, "index", source_index);
  RNA_enum_set(op->ptr, "direction", 1);  /* Down */


  return light_manager_group_move_exec(C, op);
}

/** \} */

static void light_manager_dropboxes()
{
  ListBase *lb = WM_dropboxmap_find("Light Manager", SPACE_LIGHT_MANAGER, RGN_TYPE_WINDOW);
  
  /* Light drop handler. */
  wmDropBox *drop;
  drop = WM_dropbox_add(lb,
                        "LIGHT_MANAGER_OT_drop_light",
                        light_drop_poll,
                        nullptr,  /* copy */
                        nullptr,  /* cancel */
                        nullptr); /* tooltip */
  drop->draw_in_view = light_manager_draw_drag_ghost;
  
  /* Group reordering handler. */
  drop = WM_dropbox_add(lb,
                        "LIGHT_MANAGER_OT_group_move",
                        group_drop_poll,
                        nullptr,  /* copy */
                        nullptr,  /* cancel */
                        nullptr); /* tooltip */
  drop->draw_in_view = light_manager_draw_drag_ghost;
}

/** \} */

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
  st->dropboxes = light_manager_dropboxes;
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
