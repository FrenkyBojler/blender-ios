/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 *
 * Operators to handle dropping in the 3D View.
 */

#include "AS_asset_representation.hh"

#include "BLI_listbase.h"

#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_collection_types.h"
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

static Object *asset_list_drag_find_object_to_activate(const wmDrag *drag,
                                                       blender::Span<ID *> dropped_ids)
{
  BLI_assert(BLI_listbase_count(&drag->asset_items) == dropped_ids.size());

  /* See if the active asset (the asset dragging was invoked from) is an object or a collection. In
   * case of a collection, its first object gets activated. */
  if (wmDragAsset *active_asset = WM_drag_asset_list_active_asset(drag)) {
    ID_Type idtype = active_asset->asset->get_id_type();

    if (ELEM(idtype, ID_OB, ID_GR)) {
      std::optional<int> dragged_asset_idx = WM_drag_asset_list_item_index_from_asset(
          drag, active_asset->asset);
      BLI_assert(!dragged_asset_idx || *dragged_asset_idx < dropped_ids.size());

      if (dragged_asset_idx && (*dragged_asset_idx < dropped_ids.size())) {
        BLI_assert(GS(dropped_ids[*dragged_asset_idx]->name) == idtype);

        if (GS(dropped_ids[*dragged_asset_idx]->name) == ID_OB) {
          return reinterpret_cast<Object *>(dropped_ids[*dragged_asset_idx]);
        }
        if (GS(dropped_ids[*dragged_asset_idx]->name) == ID_GR) {
          Collection *collection = reinterpret_cast<Collection *>(dropped_ids[*dragged_asset_idx]);
          if (collection->gobject.first) {
            return static_cast<Object *>(
                static_cast<CollectionObject *>(collection->gobject.first)->ob);
          }
        }
      }
    }
  }

  for (ID *dropped_id : dropped_ids) {
    if (GS(dropped_id->name) == ID_OB) {
      return reinterpret_cast<Object *>(dropped_id);
    }
    if (GS(dropped_id->name) == ID_GR) {
      Collection *collection = reinterpret_cast<Collection *>(dropped_id);
      if (collection->gobject.first) {
        return static_cast<Object *>(collection->gobject.first);
      }
    }
  }

  return nullptr;
}

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

  BKE_view_layer_synced_ensure(scene, view_layer);

  /* Select all added objects, activate the first one. */
  for (ID *id : dropped_ids) {
    if (GS(id->name) == ID_OB) {
      Object *object = reinterpret_cast<Object *>(id);
      Base *base = BKE_view_layer_base_find(view_layer, object);
      base->flag |= BASE_SELECTED;
    }
    else if (GS(id->name) == ID_GR) {
      Collection *collection = (Collection *)id;
      LISTBASE_FOREACH (CollectionObject *, cob, &collection->gobject) {
        Base *base = BKE_view_layer_base_find(view_layer, cob->ob);
        base->flag |= BASE_SELECTED;
      }
    }
  }

  Object *object_to_activate = asset_list_drag_find_object_to_activate(drag, dropped_ids);
  Base *base_to_activate = object_to_activate ?
                               BKE_view_layer_base_find(view_layer, object_to_activate) :
                               nullptr;
  if (base_to_activate) {
    BKE_view_layer_base_select_and_set_active(view_layer, base_to_activate);
    WM_main_add_notifier(NC_SCENE | ND_OB_ACTIVE, scene);
  }

  /* TODO(sergey): Only update relations for the current scene. */
  DEG_relations_tag_update(CTX_data_main(C));
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);

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

/* -------------------------------------------------------------------- */
/** \name Add asset to file operator
 *
 * Use for drag & drop.
 * \{ */

static wmOperatorStatus view3d_drop_asset_add_to_file_invoke(bContext *C,
                                                             wmOperator * /*op*/,
                                                             const wmEvent *event)
{
  const wmDrag *drag = WM_drag_get_data_from_event(event);
  if (!drag || (drag->type != WM_DRAG_ASSET)) {
    return OPERATOR_CANCELLED;
  }

  wmDragAsset *asset_drag = WM_drag_get_asset_data(drag, 0);
  WM_drag_asset_id_import(C, asset_drag, 0);

  WM_event_add_notifier(C, NC_WINDOW, nullptr);

  return OPERATOR_FINISHED;
}

void VIEW3D_OT_drop_asset_add_to_file(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add Asset to File";
  ot->description = "Import dragged asset to the file";
  ot->idname = "VIEW3D_OT_drop_asset_add_to_file";

  /* api callbacks */
  ot->invoke = view3d_drop_asset_add_to_file_invoke;
  ot->poll = ED_operator_objectmode_poll_msg;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */
