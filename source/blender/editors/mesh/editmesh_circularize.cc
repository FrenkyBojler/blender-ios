/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 */
#include "ED_screen.hh"
#include "WM_api.hh"

static wmOperatorStatus edbm_circularize_exec(bContext *C, wmOperator *op)
{
  printf("Circularize exec");
  return OPERATOR_FINISHED;
}

void MESH_OT_circularize(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "circularize";
  ot->description = "Shape boundary vertices into a circular form";
  ot->idname = "MESH_OT_circularize";

  /* API callbacks */
  ot->exec = edbm_circularize_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
