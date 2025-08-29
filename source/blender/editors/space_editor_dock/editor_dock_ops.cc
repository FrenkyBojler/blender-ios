/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup speditordock
 */

#include "BKE_context.hh"

#include "BLI_listbase.h"

#include "DNA_space_enums.h"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"

#include "ED_editor_dock.hh"
#include "editor_dock_intern.hh"

namespace blender::ed::editor_dock {

/* TODO somehow pass docked area? */
static ScrArea *lookup_docked_area(bContext *C)
{
  const bScreen *screen = CTX_wm_screen(C);
  LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
    if (area->flag & AREA_FLAG_DOCKED) {
      return area;
    }
  }
  return nullptr;
}

static const EnumPropertyItem *rna_space_ui_type_itemf(bContext *C,
                                                       PointerRNA * /*ptr*/,
                                                       PropertyRNA * /*prop*/,
                                                       bool *r_free)
{
  /* Initialize some dummy area for #rna_Area_ui_type_itemf() to work with. */
  ScrArea dummy_area = blender::dna::shallow_zero_initialize();
  /* #rna_Area_ui_type_itemf() includes #SPACE_EMTPY otherwise. */
  dummy_area.spacetype = SPACE_INFO;

  const EnumPropertyItem *item;
  int item_len;
  /* #rna_Area_ui_type_itemf() currently is fine without passing the ID pointer (which should
   * probably be a screen, if anything). */
  PointerRNA area_ptr = RNA_pointer_create_discrete(nullptr, &RNA_Area, &dummy_area);
  PropertyRNA *prop_ui_type = RNA_struct_find_property(&area_ptr, "ui_type");
  RNA_property_enum_items(C, &area_ptr, prop_ui_type, &item, &item_len, r_free);

  return item;
}

static bool add_editor_poll(bContext *C)
{
  return lookup_docked_area(C) != nullptr;
}

static wmOperatorStatus add_editor_exec(bContext *C, wmOperator *op)
{
  const int ui_type = RNA_enum_get(op->ptr, "type");
  /* See #rna_Area_ui_type_set(). */
  const eSpace_Type space_type = eSpace_Type(ui_type >> 16);
  const int subtype = ui_type & 0xffff;

  ScrArea *docked_area = lookup_docked_area(C);

  SpaceLink *new_space = add_docked_space(docked_area, space_type, subtype, CTX_data_scene(C));
  activate_docked_space(C, docked_area, new_space);
  unhide_docked_area(C, docked_area);

  return OPERATOR_FINISHED;
}

static void SCREEN_OT_editor_dock_add_editor(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Add Editor";
  ot->description = "Choose an editor to add to the editor dock";
  ot->idname = "SCREEN_OT_editor_dock_add_editor";

  /* Callbacks. */
  ot->exec = add_editor_exec;
  // ot->invoke = add_editor_invoke;
  ot->poll = add_editor_poll;

  /* TODO itemf to filter out invalid types? */
  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "type", rna_enum_space_type_items, SPACE_EMPTY, "Type", "The editor to add");
  RNA_def_property_enum_funcs_runtime(prop, nullptr, nullptr, rna_space_ui_type_itemf);
}

void register_operatortypes()
{
  WM_operatortype_append(SCREEN_OT_editor_dock_add_editor);
}

}  // namespace blender::ed::editor_dock
