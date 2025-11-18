/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 */
#include "WM_api.hh"

static wmOperatorStatus edbm_circularize_exec(bContext *C, wmOperator *op)
{
  printf("Circularize exec");
  return OPERATOR_FINISHED;
}

void MESH_OT_circularize(wmOperatorType *ot)
{
  ot->name = "Circularize";
  ot->description = "Shpe boundary vertices into a circular form";
  ot->idname = "MESH_OT_circularize";
  
  ot->exec = edbm_circularize_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}
