/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_windowmanager_enums.h"

#include "WM_api.hh"

#include "BKE_context.hh"

#include "ED_screen.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

static wmOperatorStatus sockets_sync_exec(bContext * /*C*/, wmOperator * /*op*/)
{
  printf("hello world\n");
  return OPERATOR_FINISHED;
}

void NODE_OT_sockets_sync(wmOperatorType *ot)
{
  ot->name = "Sync Sockets";
  ot->idname = "NODE_OT_sockets_sync";
  ot->description = "Update sockets to match what is actually used";

  ot->poll = ED_operator_node_editable;
  ot->exec = sockets_sync_exec;
}

}  // namespace blender::ed::space_node
