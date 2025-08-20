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

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"

#include "editor_dock_intern.hh"

namespace blender::ed::editor_dock {

static void editor_dock_draw(const bContext *C, Panel *panel)
{
  const bScreen *screen = CTX_wm_screen(C);
  uiLayout &layout = *panel->layout;

  layout.ui_units_x_set(1.5f);

  LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
    if ((area->flag & AREA_FLAG_DOCKED) == 0) {
      continue;
    }

    LISTBASE_FOREACH_BACKWARD (SpaceLink *, space, &area->spacedata) {
      uiBut *but = uiItemL_ex(&layout, "", ED_spacedata_icon(space), false, false);
      UI_but_func_quick_tooltip_set(but,
                                    [space](const uiBut *) { return ED_spacedata_name(space); });
      UI_but_drawflag_disable(but, UI_BUT_ICON_LEFT);
    }

    layout.emboss_set(ui::EmbossType::NoneOrStatus);
    layout.op_menu_enum(C, "SCREEN_OT_editor_dock_add_editor", "type", "", ICON_ADD);
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
