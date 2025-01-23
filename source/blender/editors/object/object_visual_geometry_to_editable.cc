/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"

#include "ED_screen.hh"

#include "WM_types.hh"

namespace blender::ed::object {

int visual_geometry_to_editable_exec(bContext *C, wmOperator * /*op*/)
{
  Object *ob = CTX_data_active_object(C);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_visual_geometry_to_editable(wmOperatorType *ot)
{
  ot->name = "Visual Geometry to Editable";
  ot->description = "Convert geometry and instances into editable objects and collections";
  ot->idname = "OBJECT_OT_visual_geometry_to_editable";

  ot->exec = visual_geometry_to_editable_exec;
  ot->poll = ED_operator_object_active;
}

}  // namespace blender::ed::object
