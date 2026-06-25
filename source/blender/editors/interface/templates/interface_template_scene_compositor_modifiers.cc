/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Template for building the panel layout for the scene compositor modifiers.
 */

#include "BLI_listbase.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"

#include "BLT_translation.hh"

#include "DNA_listBase.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "WM_api.hh"

#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

namespace blender::ui {

static void draw_modifier_extra_menu(bContext *C, ui::Layout *layout, void *modifier_v)
{
  Scene *scene = CTX_data_scene(C);
  SceneCompositorModifier *modifier = static_cast<SceneCompositorModifier *>(modifier_v);

  {
    PointerRNA operator_ptr = layout->op("NODE_OT_duplicate_scene_compositor_modifier",
                                         CTX_IFACE_(BLT_I18NCONTEXT_OPERATOR_DEFAULT, "Duplicate"),
                                         ICON_DUPLICATE);
    RNA_string_set(&operator_ptr, "name", modifier->name);
  }

  layout->separator();

  {
    ui::Layout &row = layout->row(false);
    PointerRNA operator_ptr = row.op("NODE_OT_scene_compositor_modifier_move_to_index",
                                     IFACE_("Move to First"),
                                     ICON_TRIA_UP,
                                     wm::OpCallContext::InvokeDefault,
                                     UI_ITEM_NONE);
    RNA_string_set(&operator_ptr, "name", modifier->name);
    RNA_int_set(&operator_ptr, "index", 0);
    row.enabled_set(modifier->previous != nullptr);
  }

  {
    ui::Layout &row = layout->row(false);
    PointerRNA operator_ptr = row.op("NODE_OT_scene_compositor_modifier_move_to_index",
                                     IFACE_("Move to Last"),
                                     ICON_TRIA_DOWN,
                                     wm::OpCallContext::InvokeDefault,
                                     UI_ITEM_NONE);
    RNA_string_set(&operator_ptr, "name", modifier->name);
    RNA_int_set(&operator_ptr, "index", scene->compositor_modifiers.count() - 1);
    row.enabled_set(modifier->next != nullptr);
  }

  layout->separator();

  PointerRNA modifier_ptr = RNA_pointer_create_discrete(
      &scene->id, RNA_SceneCompositorModifier, modifier);
  layout->prop(&modifier_ptr, "show_node_group_selector", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void draw_modifier_panel_header(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *modifier_ptr = ui::panel_custom_data_get(panel);
  ui::panel_context_pointer_set(panel, "modifier", modifier_ptr);
  SceneCompositorModifier *modifier = modifier_ptr->data_as<SceneCompositorModifier>();

  ui::Layout &icon_row = layout.row(true);
  icon_row.emboss_set(ui::EmbossType::None);
  PointerRNA set_active_operator_ptr = icon_row.op(
      "NODE_OT_set_active_scene_compositor_modifier", "", RNA_struct_ui_icon(modifier_ptr->type));
  RNA_string_set(&set_active_operator_ptr, "name", modifier->name);

  ui::Layout &buttons_row = layout.row(true);
  ui::Layout &name_row = buttons_row.row(true);

  constexpr int number_of_buttons = 3;
  const int available_space_for_name = (panel->sizex / UI_UNIT_X) - number_of_buttons;
  if (available_space_for_name > 5) {
    name_row.prop(modifier_ptr, "name", UI_ITEM_NONE, "", ICON_NONE);
  }
  else {
    buttons_row.alignment_set(ui::LayoutAlign::Right);
  }

  ui::Layout &enable_for_preview_row = buttons_row.row(true);
  enable_for_preview_row.prop(modifier_ptr, "enable_for_preview", UI_ITEM_NONE, "", ICON_NONE);

  ui::Layout &enable_for_render_row = buttons_row.row(true);
  enable_for_render_row.prop(modifier_ptr, "enable_for_render", UI_ITEM_NONE, "", ICON_NONE);

  buttons_row.menu_fn("", ICON_DOWNARROW_HLT, draw_modifier_extra_menu, modifier);

  ui::Layout &remove_row = buttons_row.row(false);
  remove_row.emboss_set(ui::EmbossType::None);
  PointerRNA remove_operator_ptr = remove_row.op(
      "NODE_OT_remove_scene_compositor_modifier", "", ICON_X);
  RNA_string_set(&remove_operator_ptr, "name", modifier->name);

  layout.separator();
}

static void reorder_modifier(bContext *C, Panel *panel, const int new_index)
{
  PointerRNA *modifier_ptr = ui::panel_custom_data_get(panel);
  SceneCompositorModifier *modifier = modifier_ptr->data_as<SceneCompositorModifier>();

  wmOperatorType *operator_type = WM_operatortype_find(
      "NODE_OT_scene_compositor_modifier_move_to_index", false);
  PointerRNA properties_ptr = WM_operator_properties_create_ptr(operator_type);
  RNA_string_set(&properties_ptr, "name", modifier->name);
  RNA_int_set(&properties_ptr, "index", new_index);
  WM_operator_name_call_ptr(
      C, operator_type, wm::OpCallContext::InvokeDefault, &properties_ptr, nullptr);
  WM_operator_properties_free(&properties_ptr);
}

static short get_modifier_expand_flag(const bContext * /*C*/, Panel *panel)
{
  PointerRNA *modifier_ptr = ui::panel_custom_data_get(panel);
  SceneCompositorModifier *modifier = modifier_ptr->data_as<SceneCompositorModifier>();
  return modifier->ui_panel_data_expansion;
}

static void set_modifier_expand_flag(const bContext * /*C*/, Panel *panel, short expand_flag)
{
  PointerRNA *modifier_ptr = ui::panel_custom_data_get(panel);
  SceneCompositorModifier *modifier = modifier_ptr->data_as<SceneCompositorModifier>();
  modifier->ui_panel_data_expansion = uiPanelDataExpansion(expand_flag);
}

static void draw_modifier_panel(const bContext *C, Panel *panel)
{
  PointerRNA *modifier_ptr = ui::panel_custom_data_get(panel);
  ui::panel_context_pointer_set(panel, "modifier", modifier_ptr);

  SceneCompositorModifier &modifier = *modifier_ptr->data_as<SceneCompositorModifier>();

  ui::Layout &layout = *panel->layout;
  layout.use_property_split_set(true);

  if (flag_is_set(modifier.flags, SceneCompositorModifierFlags::ShowNodeGroupSelector)) {
    const char *operator_name = (modifier.node_group == nullptr) ?
                                    "node.new_scene_compositor_modifier_node_group" :
                                    "node.duplicate_scene_compositor_modifier_node_group";
    template_id(&layout, C, modifier_ptr, "node_group", operator_name, nullptr, nullptr);
  }
}

static constexpr char SCENE_COMPOSITOR_MODIFIER_PANEL_IDNAME[] = "SCENE_COMPOSITOR_MODIFIER_PT";

void register_scene_compositor_modifiers_panel(ARegionType *region_type)
{
  PanelType *panel_type = MEM_new_zeroed<PanelType>(__func__);

  STRNCPY_UTF8(panel_type->idname, SCENE_COMPOSITOR_MODIFIER_PANEL_IDNAME);
  STRNCPY_UTF8(panel_type->label, "");
  STRNCPY_UTF8(panel_type->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  STRNCPY_UTF8(panel_type->active_property, "is_active");
  STRNCPY_UTF8(panel_type->context, "scene_compositor_modifiers");

  panel_type->draw_header = draw_modifier_panel_header;
  panel_type->draw = draw_modifier_panel;

  /* Give the panel the special flag that says it was built here and corresponds to a
   * modifier rather than a #PanelType. */
  panel_type->flag = PANEL_TYPE_HEADER_EXPAND | PANEL_TYPE_INSTANCED;
  panel_type->reorder = reorder_modifier;
  panel_type->get_list_data_expand_flag = get_modifier_expand_flag;
  panel_type->set_list_data_expand_flag = set_modifier_expand_flag;

  BLI_addtail(&region_type->paneltypes, panel_type);
}

static void modifier_panel_id(void * /*modifier_link*/, char *r_name)
{
  BLI_strncpy(r_name, SCENE_COMPOSITOR_MODIFIER_PANEL_IDNAME, MAX_NAME);
}

void template_scene_compositor_modifiers(Layout * /*layout*/, bContext *C)
{
  Scene *scene = CTX_data_scene(C);
  if (!scene) {
    return;
  }
  ListBaseT<SceneCompositorModifier> *modifiers = &scene->compositor_modifiers;

  ARegion *region = CTX_wm_region(C);
  const bool panels_match = panel_list_matches_data(region, modifiers, modifier_panel_id);

  if (!panels_match) {
    panels_free_instanced(C, region);
    for (SceneCompositorModifier &modifier : *modifiers) {
      /* Create custom data RNA pointer. */
      PointerRNA *modifier_ptr = MEM_new<PointerRNA>(__func__);
      *modifier_ptr = RNA_pointer_create_discrete(
          &scene->id, RNA_SceneCompositorModifier, &modifier);

      panel_add_instanced(
          C, region, &region->panels, SCENE_COMPOSITOR_MODIFIER_PANEL_IDNAME, modifier_ptr);
    }
  }
  else {
    /* Assuming there's only one group of instanced panels, update the custom data pointers. */
    Panel *panel = static_cast<Panel *>(region->panels.first);
    for (SceneCompositorModifier &modifier : *modifiers) {
      /* Move to the next instanced panel corresponding to the next modifier. */
      while ((panel->type == nullptr) || !(panel->type->flag & PANEL_TYPE_INSTANCED)) {
        panel = panel->next;
        /* There shouldn't be fewer panels than modifiers with UIs. */
        BLI_assert(panel != nullptr);
      }

      PointerRNA *modifier_ptr = MEM_new<PointerRNA>(__func__);
      *modifier_ptr = RNA_pointer_create_discrete(
          &scene->id, RNA_SceneCompositorModifier, &modifier);
      panel_custom_data_set(panel, modifier_ptr);

      panel = panel->next;
    }
  }
}

}  // namespace blender::ui
