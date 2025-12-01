/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edobj
 */

#include "object_intern.hh"

#include "BKE_context.hh"
#include "BKE_object.hh"

#include "ED_object.hh"
#include "ED_screen.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_resources.hh"

#include "DNA_object_types.h"
#include "DNA_ID.h"
#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

namespace blender::ed::object {

/* -------------------------------------------------------------------- */
/** \name Add LOD Operator
 * \{ */

static wmOperatorStatus object_lod_add_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);

  if (ob == nullptr) {
    return OPERATOR_CANCELLED;
  }

  BKE_object_lod_add(ob);

  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_ID | NA_EDITED, &ob->id);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_lod_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add LOD";
  ot->description = "Add a Distance LOD entry to the active object";
  ot->idname = "OBJECT_OT_lod_add";

  /* API callbacks */
  ot->exec = object_lod_add_exec;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Remove LOD Operator
 * \{ */

static wmOperatorStatus object_lod_remove_exec(bContext *C, wmOperator * /*op*/)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);

  if (ob == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const int index = ob->lod_items_index;

  if (index < 0 || index >= ob->lod_items_num) {
    return OPERATOR_PASS_THROUGH;
  }

  if (!BKE_object_lod_remove(ob, index)) {
    return OPERATOR_CANCELLED;
  }
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);


  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);
  WM_event_add_notifier(C, NC_ID | NA_EDITED, &ob->id);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_lod_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove LOD";
  ot->description = "Remove the active Distance LOD entry from the active object";
  ot->idname = "OBJECT_OT_lod_remove";

  /* API callbacks */
  ot->exec = object_lod_remove_exec;
  ot->poll = ED_operator_object_active_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

}  // namespace blender::ed::object
