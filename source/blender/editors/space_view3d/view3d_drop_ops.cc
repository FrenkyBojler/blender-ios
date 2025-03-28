/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 *
 * Operators to handle dropping in the 3D View.
 */

#include "BKE_context.hh"
#include "BKE_layer.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_scene_types.h"

#include "ED_outliner.hh"
#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "view3d_intern.hh"

/* -------------------------------------------------------------------- */
/** \name Drop assets operator.
 *
 * Use for drag & drop.
 * \{ */

static wmOperatorStatus view3d_drop_assets_invoke(bContext *C,
                                                  wmOperator * /*op*/,
                                                  const wmEvent *event)
{
  const wmDrag *drag = WM_drag_get_data_from_event(event);
  if (!drag || (drag->type != WM_DRAG_ASSET_LIST)) {
    return OPERATOR_CANCELLED;
  }

  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  BKE_view_layer_base_deselect_all(scene, view_layer);

  blender::Vector<ID *> dropped_ids = WM_drag_asset_list_id_import_all(C, drag, FILE_AUTOSELECT);
  if (dropped_ids.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  /* TODO(sergey): Only update relations for the current scene. */
  DEG_relations_tag_update(CTX_data_main(C));
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);

  BKE_view_layer_synced_ensure(scene, view_layer);
  /* Select first object in the list. */
  for (ID *id : dropped_ids) {
    if (GS(id->name) != ID_OB) {
      continue;
    }
    Base *base = BKE_view_layer_base_find(view_layer, (Object *)id);
    if (base != nullptr) {
      BKE_view_layer_base_select_and_set_active(view_layer, base);
      WM_main_add_notifier(NC_SCENE | ND_OB_ACTIVE, scene);
      break;
    }
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_SELECT);
  ED_outliner_select_sync_from_object_tag(C);

  /* Make sure the depsgraph is evaluated so the new object's transforms are up-to-date.
   * The evaluated #Object::object_to_world() will be copied back to the original object
   * and used below. */
  CTX_data_ensure_evaluated_depsgraph(C);

  return OPERATOR_FINISHED;
}

void VIEW3D_OT_drop_assets(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Drop Assets";
  ot->description =
      "Import dragged assets to the file, add objects and collections to the active collection";
  ot->idname = "VIEW3D_OT_drop_assets";

  /* api callbacks */
  ot->invoke = view3d_drop_assets_invoke;
  ot->poll = ED_operator_objectmode_poll_msg;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */
