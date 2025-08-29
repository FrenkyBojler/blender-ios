/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup speditordock
 */

#include "BKE_screen.hh"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"

#include "BLT_translation.hh"

#include "DNA_screen_types.h"

#include "ED_screen.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"

#include "ED_editor_dock.hh"
#include "editor_dock_intern.hh"

namespace blender::ed::editor_dock {

static void editor_dock_draw(const bContext *C, Panel *panel)
{
  const bScreen *screen = CTX_wm_screen(C);
  uiLayout &layout = *panel->layout;

  layout.ui_units_x_set(1.5f);
  layout.emboss_set(ui::EmbossType::NoneOrStatus);

  bool is_first_area = true;

  LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
    if (!area->docked) {
      continue;
    }

    if (!is_first_area) {
      layout.separator_spacer();
    }
    is_first_area = false;

    const bool is_visible = (area->flag & AREA_FLAG_HIDDEN) == 0;

    LISTBASE_FOREACH_BACKWARD (LinkData *, node, &area->docked_spaces_ordered) {
      SpaceLink *space = static_cast<SpaceLink *>(node->data);
      const bool is_active = is_visible && (space == area->spacedata.first);

      uiLayout &row = layout.row(false);
      if (is_active) {
        row.emboss_set(ui::EmbossType::Emboss);
      }

      uiBut *but = uiDefIconBut(layout.block(),
                                ButType::Tab,
                                0,
                                ED_spacedata_icon(space),
                                0,
                                0,
                                UI_UNIT_X * 1.5f,
                                UI_UNIT_Y * 1.5f,
                                nullptr,
                                0,
                                0,
                                "");
      UI_but_func_pushed_state_set(but, [is_active](const uiBut &) { return is_active; });
      UI_but_func_set(but, [area, space](bContext &C) { toggle_docked_space(&C, area, space); });
      UI_but_func_quick_tooltip_set(but,
                                    [space](const uiBut *) { return ED_spacedata_name(space); });
      UI_but_drawflag_disable(but, UI_BUT_ICON_LEFT);

      ui::block_layout_set_current(layout.block(), &layout);
    }

    PointerRNA op_ptr = layout.op_menu_enum(
        C, "SCREEN_OT_editor_dock_add_editor", "type", "", ICON_ADD);
    RNA_int_set(&op_ptr, "position", area->docked->position);
  }
}

void main_region_panels_register(ARegionType *art)
{
  PanelType *pt;

  pt = MEM_callocN<PanelType>("spacetype editor dock main region panels");
  STRNCPY_UTF8(pt->idname, "EDITORDOCK_PT_editor_dock");
  STRNCPY_UTF8(pt->label, N_("Editor Dock"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->flag = PANEL_TYPE_NO_HEADER;
  pt->draw = editor_dock_draw;
  BLI_addtail(&art->paneltypes, pt);
}

}  // namespace blender::ed::editor_dock
