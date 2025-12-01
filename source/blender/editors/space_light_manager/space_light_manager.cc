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
};

static const LightManagerColumnLayout light_manager_columns[] = {
    {"", 0.04f},          /* Drag handle (centered) */
    {"", 0.03f},          /* Light type icon (centered) */
    {"Name", 0.43f},      /* Object name */
    {"Color", 0.22f},     /* Light color */
    {"Power", 0.18f},     /* Light energy */
    {"Visibility", 0.10f} /* Viewport/render toggles + remove */
};

struct LightManagerRow {
  SpaceLightManagerGroup *group; /* Optional, nullptr for ungrouped header/rows. */
  Object *ob;                    /* Light object, nullptr for pure group header rows. */
  bool is_group_header;
};

class LightManagerDrawer {
 public:
  Scene *scene;
  ARegion *region;
  blender::Vector<LightManagerRow> rows;

  int top_row_height;
  int row_height;
  int rows_y_top;

  LightManagerDrawer(Scene *scene, ARegion *region, int start_y)
      : scene(scene), region(region)
  {
    top_row_height = int(UI_UNIT_Y * 1.1f);
    row_height = int(UI_UNIT_Y * 1.2f);
    rows_y_top = start_y;
  }

  int table_left_x() const
  {
    return 10; /* Match layout x offset above. */
  }

  int table_width() const
  {
    return region->winx - 20; /* Same margin as block_layout. */
  }

  int column_width_px(int column_index) const
  {
    const float fraction = light_manager_columns[column_index].width_fraction;
    return int(float(table_width()) * fraction);
  }

  int column_x_px(int column_index) const
  {
    int x = table_left_x();
    for (int i = 0; i < column_index; i++) {
      x += column_width_px(i);
    }
    return x;
  }

  void draw_rows(const bContext *C, uiBlock *block) const
  {
    using namespace blender;

    int y = rows_y_top;

    SpaceLightManagerGroup *current_group = nullptr;
    bool ungrouped_header_drawn = false;
    int group_y_top = 0;
    int group_row_count = 0; /* header + data rows */

    auto flush_group_background = [&](SpaceLightManagerGroup * /*group*/, int y_top, int rows) {
      if (rows <= 0) {
        return;
      }
      rctf rect;
      rect.xmin = float(table_left_x());
      rect.xmax = float(table_left_x() + table_width());
      rect.ymax = float(y_top + top_row_height);
      rect.ymin = float(y_top - rows * row_height);

      float col[4];
      UI_GetThemeColor4fv(TH_BACK, col);
      UI_draw_roundbox_4fv(&rect, true, 0.0f, col);
    };

    auto draw_header_row = [&](int y_header) {
      for (int col = 0; col < int(eLightManagerColumn::Count); col++) {
        const char *label = light_manager_columns[col].label;
        if (label[0] == '\0') {
          continue;
        }

        const int x = column_x_px(col);
        const int w = column_width_px(col);

        uiDefBut(block,
                 ButType::Label,
                 label,
                 x,
                 y_header,
                 short(w),
                 short(top_row_height),
                 nullptr,
                 0.0f,
                 0.0f,
                 std::nullopt);
      }
    };

    for (const LightManagerRow &row_data : rows) {
      if (row_data.ob == nullptr) {
        continue;
      }

      /* Draw a header row when entering a new group, or once for ungrouped lights. */
      if (row_data.group != current_group) {
        /* Finish background for previous group. */
        flush_group_background(current_group, group_y_top, group_row_count);

        current_group = row_data.group;
        group_y_top = y;
        group_row_count = 0;

        if (current_group != nullptr || !ungrouped_header_drawn) {
          draw_header_row(y);
          y -= row_height;
          group_row_count++;

          if (current_group == nullptr) {
            ungrouped_header_drawn = true;
          }
        }
      }

      PointerRNA ob_ptr = RNA_pointer_create_discrete(&scene->id, &RNA_Object, row_data.ob);
      PointerRNA light_ptr = RNA_pointer_get(&ob_ptr, "data");

      /* Drag column. */
      {
        const int col = int(eLightManagerColumn::Drag);
        const int x = column_x_px(col);
        const int w = column_width_px(col);

        uiBut *drag_but = uiDefIconBut(block,
                                       ButType::Label,
                                       ICON_GRIP,
                                       x,
                                       y,
                                       short(w),
                                       short(row_height),
                                       nullptr,
                                       0.0f,
                                       0.0f,
                                       std::nullopt);
        UI_but_drag_set_id(drag_but, &row_data.ob->id);
      }

      /* Icon column. */
      {
        const int col = int(eLightManagerColumn::Icon);
        const int x = column_x_px(col);
        const int w = column_width_px(col);

        int type_icon = ICON_LIGHT;
        Light *light = static_cast<Light *>(row_data.ob->data);
        switch (light->type) {
          case LA_LOCAL: type_icon = ICON_LIGHT_POINT; break;
          case LA_SUN: type_icon = ICON_LIGHT_SUN; break;
          case LA_SPOT: type_icon = ICON_LIGHT_SPOT; break;
          case LA_AREA: type_icon = ICON_LIGHT_AREA; break;
        }

        uiDefIconBut(block,
                     ButType::Label,
                     type_icon,
                     x,
                     y,
                     short(w),
                     short(row_height),
                     nullptr,
                     0.0f,
                     0.0f,
                     std::nullopt);
      }

      /* Name column. */
      {
        const int col = int(eLightManagerColumn::Name);
        const int x = column_x_px(col);
        const int w = column_width_px(col);

        PropertyRNA *prop = RNA_struct_find_property(&ob_ptr, "name");
        if (prop != nullptr) {
          uiDefButR_prop(block,
                         ButType::Text,
                         std::nullopt,
                         x,
                         y,
                         short(w),
                         short(row_height),
                         &ob_ptr,
                         prop,
                         -1,
                         0.0f,
                         0.0f,
                         std::nullopt);
        }
      }

      /* Color column. */
      {
        const int col = int(eLightManagerColumn::Color);
        const int x = column_x_px(col);
        const int w = column_width_px(col);

        PropertyRNA *prop = RNA_struct_find_property(&light_ptr, "color");
        if (prop != nullptr) {
          uiDefButR_prop(block,
                         ButType::Color,
                         std::nullopt,
                         x,
                         y,
                         short(w),
                         short(row_height),
                         &light_ptr,
                         prop,
                         -1,
                         0.0f,
                         0.0f,
                         std::nullopt);
        }
      }

      /* Power column. */
      {
        const int col = int(eLightManagerColumn::Power);
        const int x = column_x_px(col);
        const int w = column_width_px(col);

        PropertyRNA *prop = RNA_struct_find_property(&light_ptr, "energy");
        if (prop != nullptr) {
          uiDefButR_prop(block,
                         ButType::NumSlider,
                         std::nullopt,
                         x,
                         y,
                         short(w),
                         short(row_height),
                         &light_ptr,
                         prop,
                         -1,
                         0.0f,
                         0.0f,
                         std::nullopt);
        }
      }

      /* Visibility column: hide_viewport, hide_render, remove. */
      {
        const int col = int(eLightManagerColumn::Visibility);
        int x = column_x_px(col);
        const int w = column_width_px(col);
        const int icon_w = UI_UNIT_X;

        PropertyRNA *prop_hide_view = RNA_struct_find_property(&ob_ptr, "hide_viewport");
        if (prop_hide_view != nullptr) {
          uiDefButR_prop(block,
                         ButType::IconToggle,
                         std::nullopt,
                         x,
                         y,
                         short(icon_w),
                         short(row_height),
                         &ob_ptr,
                         prop_hide_view,
                         -1,
                         0.0f,
                         0.0f,
                         std::nullopt);
        }
        x += icon_w;

        PropertyRNA *prop_hide_render = RNA_struct_find_property(&ob_ptr, "hide_render");
        if (prop_hide_render != nullptr) {
          uiDefButR_prop(block,
                         ButType::IconToggle,
                         std::nullopt,
                         x,
                         y,
                         short(icon_w),
                         short(row_height),
                         &ob_ptr,
                         prop_hide_render,
                         -1,
                         0.0f,
                         0.0f,
                         std::nullopt);
        }
        x += icon_w;

        const int remaining_w = std::max(0, w - 2 * icon_w);
        uiBut *remove_but = uiDefIconButO(block,
                                           ButType::But,
                                           "LIGHT_MANAGER_OT_light_remove_from_group",
                                           blender::wm::OpCallContext::InvokeDefault,
                                           ICON_X,
                                           x,
                                           y,
                                           short(remaining_w),
                                           short(row_height),
                                           std::nullopt);
        if (remove_but != nullptr) {
          UI_but_operator_ptr_ensure(remove_but);
          RNA_string_set(remove_but->opptr, "object_name", row_data.ob->id.name + 2);
        }
      }

      y -= row_height;
      group_row_count++;
    }

    /* Flush background for last group. */
    flush_group_background(current_group, group_y_top, group_row_count);
  }
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

/* Draw the table header row with pixel-precise column alignment. */
static void draw_light_table_header(uiBlock *block,
                                    const ColumnLayout columns[int(eLightManagerColumn::Count)],
                                    int y,
                                    int header_height)
{
  for (int col = 0; col < int(eLightManagerColumn::Count); col++) {
    const char *label = light_manager_columns[col].label;
    if (label[0] == '\0') {
      continue;
    }

    const int x = columns[col].x;
    const int w = columns[col].width;

    uiDefBut(block,
             ButType::Label,
             label,
             x,
             y,
             short(w),
             short(header_height),
             nullptr,
             0.0f,
             0.0f,
             std::nullopt);
  }
}

/* Draw a single light row with pixel-precise column alignment and centered elements. */
static void draw_light_table_row(const bContext *C,
                                 uiBlock *block,
                                 Scene *scene,
                                 Object *ob,
                                 const ColumnLayout columns[int(eLightManagerColumn::Count)],
                                 int y,
                                 int row_height)
{
  PointerRNA ob_ptr = RNA_pointer_create_discrete(&scene->id, &RNA_Object, ob);
  PointerRNA light_ptr = RNA_pointer_get(&ob_ptr, "data");

  /* Drag column */
  {
    const int col = int(eLightManagerColumn::Drag);
    const int x = columns[col].x;
    const int w = columns[col].width;

    /* Center the icon in the column */
    const int icon_w = UI_UNIT_X;
    const int padding = (w - icon_w) / 2;

    uiBut *drag_but = uiDefIconBut(block,
                                   ButType::Label,
                                   ICON_GRIP,
                                   x + padding,
                                   y,
                                   short(icon_w),
                                   short(row_height),
                                   nullptr,
                                   0.0f,
                                   0.0f,
                                   std::nullopt);
    UI_but_drag_set_id(drag_but, &ob->id);
  }

  /* Icon column */
  {
    const int col = int(eLightManagerColumn::Icon);
    const int x = columns[col].x;
    const int w = columns[col].width;

    Light *light = static_cast<Light *>(ob->data);
    int type_icon = ICON_LIGHT;
    switch (light->type) {
      case LA_LOCAL: type_icon = ICON_LIGHT_POINT; break;
      case LA_SUN: type_icon = ICON_LIGHT_SUN; break;
      case LA_SPOT: type_icon = ICON_LIGHT_SPOT; break;
      case LA_AREA: type_icon = ICON_LIGHT_AREA; break;
    }

    /* Center the icon in the column */
    const int icon_w = UI_UNIT_X;
    const int padding = (w - icon_w) / 2;

    uiDefIconBut(block,
                 ButType::Label,
                 type_icon,
                 x + padding,
                 y,
                 short(icon_w),
                 short(row_height),
                 nullptr,
                 0.0f,
                 0.0f,
                 std::nullopt);
  }

  /* Name column */
  {
    const int col = int(eLightManagerColumn::Name);
    const int x = columns[col].x;
    const int w = columns[col].width;

    PropertyRNA *prop = RNA_struct_find_property(&ob_ptr, "name");
    if (prop != nullptr) {
      uiDefButR_prop(block,
                     ButType::Text,
                     std::nullopt,
                     x,
                     y,
                     short(w),
                     short(row_height),
                     &ob_ptr,
                     prop,
                     -1,
                     0.0f,
                     0.0f,
                     std::nullopt);
    }
  }

  /* Color column */
  {
    const int col = int(eLightManagerColumn::Color);
    const int x = columns[col].x;
    const int w = columns[col].width;

    PropertyRNA *prop = RNA_struct_find_property(&light_ptr, "color");
    if (prop != nullptr) {
      uiDefButR_prop(block,
                     ButType::Color,
                     std::nullopt,
                     x,
                     y,
                     short(w),
                     short(row_height),
                     &light_ptr,
                     prop,
                     -1,
                     0.0f,
                     0.0f,
                     std::nullopt);
    }
  }

  /* Power column */
  {
    const int col = int(eLightManagerColumn::Power);
    const int x = columns[col].x;
    const int w = columns[col].width;

    PropertyRNA *prop = RNA_struct_find_property(&light_ptr, "energy");
    if (prop != nullptr) {
      uiDefButR_prop(block,
                     ButType::NumSlider,
                     std::nullopt,
                     x,
                     y,
                     short(w),
                     short(row_height),
                     &light_ptr,
                     prop,
                     -1,
                     0.0f,
                     0.0f,
                     std::nullopt);
    }
  }

  /* Visibility column: hide_viewport, hide_render, remove */
  {
    const int col = int(eLightManagerColumn::Visibility);
    int x = columns[col].x;
    const int w = columns[col].width;
    const int icon_w = UI_UNIT_X;

    PropertyRNA *prop_hide_view = RNA_struct_find_property(&ob_ptr, "hide_viewport");
    if (prop_hide_view != nullptr) {
      uiDefButR_prop(block,
                     ButType::IconToggle,
                     std::nullopt,
                     x,
                     y,
                     short(icon_w),
                     short(row_height),
                     &ob_ptr,
                     prop_hide_view,
                     -1,
                     0.0f,
                     0.0f,
                     std::nullopt);
    }
    x += icon_w;

    PropertyRNA *prop_hide_render = RNA_struct_find_property(&ob_ptr, "hide_render");
    if (prop_hide_render != nullptr) {
      uiDefButR_prop(block,
                     ButType::IconToggle,
                     std::nullopt,
                     x,
                     y,
                     short(icon_w),
                     short(row_height),
                     &ob_ptr,
                     prop_hide_render,
                     -1,
                     0.0f,
                     0.0f,
                     std::nullopt);
    }
    x += icon_w;

    const int remaining_w = std::max(0, w - 2 * icon_w);
    uiBut *remove_but = uiDefIconButO(block,
                                       ButType::But,
                                       "LIGHT_MANAGER_OT_light_remove_from_group",
                                       blender::wm::OpCallContext::InvokeDefault,
                                       ICON_X,
                                       x,
                                       y,
                                       short(remaining_w),
                                       short(row_height),
                                       std::nullopt);
    if (remove_but != nullptr) {
      UI_but_operator_ptr_ensure(remove_but);
      RNA_string_set(remove_but->opptr, "object_name", ob->id.name + 2);
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
      set_light_group(ob, light_manager_default_group_name());
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

  /* Get scene context. */
  Scene *scene = CTX_data_scene(C);

  SpaceLightManager *space_lm = CTX_wm_space_light_manager(C);

  /* Create UI block. */
  uiBlock *block = UI_block_begin(C, region, __func__, ui::EmbossType::Emboss);

  /* Simple layout at the top for the add-group button, keep existing style. */
  ui::Layout &layout = ui::block_layout(block,
                                         ui::LayoutDirection::Vertical,
                                         ui::LayoutType::Panel,
                                         10,
                                         region->winy - 10,
                                         region->winx - 20,
                                         region->winy,
                                         0,
                                         UI_style_get());

  ui::Layout &button_row = layout.row(false);
  button_row.op("LIGHT_MANAGER_OT_group_add", "+ Add New Group", ICON_ADD);

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
    
    /* Skip lights that don't have a group (not in Light Manager) */
    if (group_name == nullptr) {
      continue;
    }
    
    grouped_lights.lookup_or_add_default(group_name).append(ob);
  }

  /* Draw custom groups first. Always draw the group box once it exists,
   * even if there are currently no lights assigned to it. */
  if (space_lm && space_lm->groups.first) {
    LISTBASE_FOREACH (SpaceLightManagerGroup *, group, &space_lm->groups) {
      blender::Vector<Object *> *group_lights = grouped_lights.lookup_ptr(group->name);

      layout.separator();
      ui::Layout &group_box = layout.box();

      /* Track group position for drop detection. */
      if (space_lm->runtime) {
        /* We need to get the Y position of this group box.
         * Since we can't easily get exact coords during layout,
         * we'll use an approximation based on iteration order. */
        int group_idx = BLI_findindex(&space_lm->groups, group);
        
        /* Estimate Y position - will be refined after layout resolve */
        GroupBounds bounds;
        bounds.group_name = group->name;
        bounds.index = group_idx;
        bounds.y_min = 0;  /* Will be updated */
        bounds.y_max = 0;  /* Will be updated */
        
        /* For now, just mark that this group exists at this index */
        if (group_idx < space_lm->runtime->group_bounds.size()) {
          space_lm->runtime->group_bounds[group_idx] = bounds;
        } else {
          space_lm->runtime->group_bounds.append(bounds);
        }
      }

      /* Group header. */
      ui::Layout &group_header = group_box.row(false);

      /* Drag handle for group reordering. */
      uiBlock *block_ptr = group_header.block();
      uiBut *drag_but = uiDefIconBut(block_ptr,
                                     ButType::Label,
                                     ICON_GRIP,
                                     0, 0,
                                     UI_UNIT_X, UI_UNIT_Y,
                                     nullptr, 0.0f, 0.0f,
                                     std::nullopt);
      UI_but_drag_set_name(drag_but, group->name);
      
      /* Collapse/expand button. */
      int group_index = BLI_findindex(&space_lm->groups, group);
      int icon = (group->flag & SPACE_LIGHT_MANAGER_GROUP_COLLAPSED) ? 
                 ICON_DISCLOSURE_TRI_RIGHT : ICON_DISCLOSURE_TRI_DOWN;
      PointerRNA op_ptr = group_header.op("LIGHT_MANAGER_OT_group_toggle", "", icon);
      RNA_int_set(&op_ptr, "index", group_index);
      
      /* Group name (click to rename). */
      PointerRNA rename_op = group_header.op("LIGHT_MANAGER_OT_group_rename", group->name, ICON_NONE);
      if (rename_op.type != nullptr) {
        RNA_int_set(&rename_op, "index", group_index);
      }

      /* Add a light to this group via popup. */
      PointerRNA assign_op = group_header.op(
          "LIGHT_MANAGER_OT_add_light", IFACE_("Add light"), ICON_LIGHT);
      if (assign_op.type != nullptr) {
        RNA_int_set(&assign_op, "index", group_index);

        uiBlock *assign_block = group_header.block();
        uiBut *assign_but = assign_block->last_but();
        const uchar white[4] = {255, 255, 255, 255};
        UI_but_color_set(assign_but, white);
      }

      /* Reorder groups (up/down arrows). */
      PointerRNA move_up = group_header.op("LIGHT_MANAGER_OT_group_move", "", ICON_TRIA_UP);
      if (move_up.type != nullptr) {
        RNA_int_set(&move_up, "index", group_index);
        RNA_enum_set(&move_up, "direction", LIGHT_MANAGER_GROUP_MOVE_UP);
      }
      PointerRNA move_down = group_header.op(
          "LIGHT_MANAGER_OT_group_move", "", ICON_TRIA_DOWN);
      if (move_down.type != nullptr) {
        RNA_int_set(&move_down, "index", group_index);
        RNA_enum_set(&move_down, "direction", LIGHT_MANAGER_GROUP_MOVE_DOWN);
      }

      /* Toggle visibility for all lights in this group (viewport/render). */
      PointerRNA vis_view = group_header.op(
          "LIGHT_MANAGER_OT_group_toggle_visibility", "", ICON_RESTRICT_VIEW_OFF);
      if (vis_view.type != nullptr) {
        RNA_int_set(&vis_view, "index", group_index);
        RNA_enum_set(&vis_view, "mode", LIGHT_MANAGER_GROUP_VISIBILITY_VIEWPORT);
      }
      PointerRNA vis_rend = group_header.op(
          "LIGHT_MANAGER_OT_group_toggle_visibility", "", ICON_RESTRICT_RENDER_OFF);
      if (vis_rend.type != nullptr) {
        RNA_int_set(&vis_rend, "index", group_index);
        RNA_enum_set(&vis_rend, "mode", LIGHT_MANAGER_GROUP_VISIBILITY_RENDER);
      }

      /* Delete button. */
      PointerRNA del_op = group_header.op("LIGHT_MANAGER_OT_group_delete", "", ICON_X);
      RNA_int_set(&del_op, "index", group_index);

      layout.separator();

      /* Draw lights in this group if expanded directly inside the group box,
       * so the panel background expands together with the data. */
      if (!(group->flag & SPACE_LIGHT_MANAGER_GROUP_COLLAPSED)) {
        if (group_lights && !group_lights->is_empty()) {
          /* Calculate table dimensions and heights */
          const int header_height = int(UI_UNIT_Y * 1.1f);
          const int row_height = int(UI_UNIT_Y * 1.2f);
          const int table_x_start = 5;  /* Minimal padding inside the box */
          const int table_width = region->winx - 30;  /* Box width minus padding */
          
          /* Calculate column positions once for pixel-precise alignment */
          ColumnLayout columns[int(eLightManagerColumn::Count)];
          calculate_column_positions(table_width, table_x_start, columns);
          
          /* Table header: Drag | Icon | Name | Color | Power | Visibility */
          ui::Layout &header_row = group_box.row(false);
          header_row.use_property_split_set(false);
          header_row.use_property_decorate_set(false);

          uiBlock *header_block = header_row.block();
          
          /* Draw header labels at exact positions */
          for (int col = 0; col < int(eLightManagerColumn::Count); col++) {
            const char *label = light_manager_columns[col].label;
            if (label[0] == '\0') {
              continue;
            }
            
            uiDefBut(header_block,
                     ButType::Label,
                     label,
                     columns[col].x,
                     0,  /* Y will be set by layout */
                     short(columns[col].width),
                     short(header_height),
                     nullptr,
                     0.0f,
                     0.0f,
                     std::nullopt);
          }

          /* Draw each light row */
          for (Object *ob : *group_lights) {
            ui::Layout &row = group_box.row(false);
            row.use_property_split_set(false);
            row.use_property_decorate_set(false);
            
            uiBlock *row_block = row.block();
            
            /* Draw the row at exact column positions */
            PointerRNA ob_ptr = RNA_pointer_create_discrete(&scene->id, &RNA_Object, ob);
            PointerRNA light_ptr = RNA_pointer_get(&ob_ptr, "data");

            /* Drag column */
            {
              const int col = int(eLightManagerColumn::Drag);
              const int icon_w = UI_UNIT_X;
              const int padding = (columns[col].width - icon_w) / 2;

              uiBut *drag_but = uiDefIconBut(row_block,
                                             ButType::Label,
                                             ICON_GRIP,
                                             columns[col].x + padding,
                                             0,
                                             short(icon_w),
                                             short(row_height),
                                             nullptr,
                                             0.0f,
                                             0.0f,
                                             std::nullopt);
              UI_but_drag_set_id(drag_but, &ob->id);
            }

            /* Icon column */
            {
              const int col = int(eLightManagerColumn::Icon);
              Light *light = static_cast<Light *>(ob->data);
              int type_icon = ICON_LIGHT;
              switch (light->type) {
                case LA_LOCAL: type_icon = ICON_LIGHT_POINT; break;
                case LA_SUN: type_icon = ICON_LIGHT_SUN; break;
                case LA_SPOT: type_icon = ICON_LIGHT_SPOT; break;
                case LA_AREA: type_icon = ICON_LIGHT_AREA; break;
              }

              const int icon_w = UI_UNIT_X;
              const int padding = (columns[col].width - icon_w) / 2;

              uiDefIconBut(row_block,
                           ButType::Label,
                           type_icon,
                           columns[col].x + padding,
                           0,
                           short(icon_w),
                           short(row_height),
                           nullptr,
                           0.0f,
                           0.0f,
                           std::nullopt);
            }

            /* Name column */
            {
              const int col = int(eLightManagerColumn::Name);
              PropertyRNA *prop = RNA_struct_find_property(&ob_ptr, "name");
              if (prop != nullptr) {
                uiDefButR_prop(row_block,
                               ButType::Text,
                               std::nullopt,
                               columns[col].x,
                               0,
                               short(columns[col].width),
                               short(row_height),
                               &ob_ptr,
                               prop,
                               -1,
                               0.0f,
                               0.0f,
                               std::nullopt);
              }
            }

            /* Color column */
            {
              const int col = int(eLightManagerColumn::Color);
              PropertyRNA *prop = RNA_struct_find_property(&light_ptr, "color");
              if (prop != nullptr) {
                uiDefButR_prop(row_block,
                               ButType::Color,
                               std::nullopt,
                               columns[col].x,
                               0,
                               short(columns[col].width),
                               short(row_height),
                               &light_ptr,
                               prop,
                               -1,
                               0.0f,
                               0.0f,
                               std::nullopt);
              }
            }

            /* Power column */
            {
              const int col = int(eLightManagerColumn::Power);
              PropertyRNA *prop = RNA_struct_find_property(&light_ptr, "energy");
              if (prop != nullptr) {
                uiDefButR_prop(row_block,
                               ButType::NumSlider,
                               std::nullopt,
                               columns[col].x,
                               0,
                               short(columns[col].width),
                               short(row_height),
                               &light_ptr,
                               prop,
                               -1,
                               0.0f,
                               0.0f,
                               std::nullopt);
              }
            }

            /* Visibility column */
            {
              const int col = int(eLightManagerColumn::Visibility);
              int x = columns[col].x;
              const int icon_w = UI_UNIT_X;

              PropertyRNA *prop_hide_view = RNA_struct_find_property(&ob_ptr, "hide_viewport");
              if (prop_hide_view != nullptr) {
                uiDefButR_prop(row_block,
                               ButType::IconToggle,
                               std::nullopt,
                               x,
                               0,
                               short(icon_w),
                               short(row_height),
                               &ob_ptr,
                               prop_hide_view,
                               -1,
                               0.0f,
                               0.0f,
                               std::nullopt);
              }
              x += icon_w;

              PropertyRNA *prop_hide_render = RNA_struct_find_property(&ob_ptr, "hide_render");
              if (prop_hide_render != nullptr) {
                uiDefButR_prop(row_block,
                               ButType::IconToggle,
                               std::nullopt,
                               x,
                               0,
                               short(icon_w),
                               short(row_height),
                               &ob_ptr,
                               prop_hide_render,
                               -1,
                               0.0f,
                               0.0f,
                               std::nullopt);
              }
              x += icon_w;

              /* Make remove button square like the other icon buttons */
              uiBut *remove_but = uiDefIconButO(row_block,
                                                 ButType::But,
                                                 "LIGHT_MANAGER_OT_light_remove_from_group",
                                                 blender::wm::OpCallContext::InvokeDefault,
                                                 ICON_X,
                                                 x,
                                                 0,
                                                 short(icon_w),
                                                 short(row_height),
                                                 std::nullopt);
              if (remove_but != nullptr) {
                UI_but_operator_ptr_ensure(remove_but);
                RNA_string_set(remove_but->opptr, "object_name", ob->id.name + 2);
              }
            }
          }
        }
        else {
          /* Empty group placeholder for better feedback. */
          ui::Layout &empty_row = group_box.row(false);
          empty_row.label(IFACE_("No lights in this group"), ICON_INFO);
        }
      }
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
  WM_operatortype_append(LIGHT_MANAGER_OT_assign_selected_to_group);
  WM_operatortype_append(LIGHT_MANAGER_OT_add_light);
  WM_operatortype_append(LIGHT_MANAGER_OT_group_move);
  WM_operatortype_append(LIGHT_MANAGER_OT_group_toggle_visibility);
  WM_operatortype_append(LIGHT_MANAGER_OT_group_rename);
  WM_operatortype_append(LIGHT_MANAGER_OT_light_remove_from_group);
  WM_operatortype_append(LIGHT_MANAGER_OT_drop_light);
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

  /* Clear old bounds and prepare for simple detection based on group count */
  int num_groups = BLI_listbase_count(&space_lm->groups);
  if (num_groups == 0) {
    return 0;
  }

  /* Use region pixel coordinates directly since View2D isn't initialized */
  int mouse_y = event->mval[1];  /* Y coordinate in region space */
  int region_height = region->winy;
  

  
  /* Simple heuristic: divide region height by number of groups
   * Groups are drawn from top to bottom, so:
   * - Group 0 is at the top (high Y values)
   * - Group N-1 is at the bottom (low Y values)
   * Note: Y=0 is at bottom of region, Y=region_height is at top */
  
  float group_height_estimate = (float)region_height / (float)(num_groups + 1);  /* +1 for header/padding */
  
  /* Start from top of region */
  float current_y_top = region_height;
  
  for (int i = 0; i < num_groups; i++) {
    float group_top = current_y_top;
    float group_bottom = current_y_top - group_height_estimate;
    
  
    
    /* Check if mouse is in this group's area */
    if (mouse_y <= group_top && mouse_y >= group_bottom) {
    
      return i;
    }
    
    current_y_top = group_bottom;
  }
  
  /* Default to last group if below all */

  return num_groups - 1;
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
  


  /* Assign the light to the group using the object's light group property. */
  set_light_group(ob, group->name);

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
  WM_dropbox_add(lb,
                 "LIGHT_MANAGER_OT_drop_light",
                 light_drop_poll,
                 nullptr,  /* copy */
                 nullptr,  /* cancel */
                 nullptr); /* tooltip */
  
  /* Group reordering handler. */
  WM_dropbox_add(lb,
                 "LIGHT_MANAGER_OT_group_move",
                 group_drop_poll,
                 nullptr,  /* copy */
                 nullptr,  /* cancel */
                 nullptr); /* tooltip */
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
