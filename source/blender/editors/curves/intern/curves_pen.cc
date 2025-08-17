/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 * Operator for creating bézier splines in Grease Pencil.
 */

#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_curves.hh"
#include "BKE_curves_utils.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_material.hh"
#include "BKE_report.hh"

#include "BLI_array_utils.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "DEG_depsgraph.hh"

#include "DNA_material_types.h"

#include "ED_curves.hh"
#include "ED_grease_pencil.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "UI_resources.hh"

namespace blender::ed::curves {

static const EnumPropertyItem prop_handle_types[] = {
    {BEZIER_HANDLE_AUTO, "AUTO", 0, "Auto", ""},
    {BEZIER_HANDLE_VECTOR, "VECTOR", 0, "Vector", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

enum class PenModal : int8_t {
  /* Move the handles of the adjacent control point. */
  MoveHandle = 0,
  /* Move the entire point even if only the handles are selected. */
  MoveEntire = 1,
  /* Snap the handles to multiples of 45 degrees. */
  SnapAngle = 2,
};

enum class ElementMode : int8_t {
  None = 0,
  Point = 1,
  Edge = 2,
  HandleLeft = 3,
  HandleRight = 4,
};

/* Invoke handler: Initialize the operator. */
static wmOperatorStatus curves_pen_invoke(bContext * /*C*/,
                                          wmOperator *op,
                                          const wmEvent * /*event*/)
{
  /* If in tools region, wait till we get to the main (3D-space)
   * region before allowing drawing to take place. */
  op->flag |= OP_IS_MODAL_CURSOR_REGION;

  return OPERATOR_RUNNING_MODAL;
}

/* Modal handler: Events handling during interactive part. */
static wmOperatorStatus curves_pen_modal(bContext * /*C*/,
                                         wmOperator * /*op*/,
                                         const wmEvent * /*event*/)
{
  /* Still running... */
  return OPERATOR_RUNNING_MODAL;
}

void pen_tool_common_props(wmOperatorType *ot)
{
  WM_operator_properties_mouse_select(ot);

  RNA_def_boolean(ot->srna,
                  "extrude_point",
                  false,
                  "Extrude Point",
                  "Add a point connected to the last selected point");
  RNA_def_enum(ot->srna,
               "extrude_handle",
               prop_handle_types,
               BEZIER_HANDLE_VECTOR,
               "Extrude Handle Type",
               "Type of the extruded handle");
  RNA_def_boolean(ot->srna, "delete_point", false, "Delete Point", "Delete an existing point");
  RNA_def_boolean(
      ot->srna, "insert_point", false, "Insert Point", "Insert Point into a curve segment");
  RNA_def_boolean(ot->srna, "move_segment", false, "Move Segment", "Delete an existing point");
  RNA_def_boolean(
      ot->srna, "select_point", false, "Select Point", "Select a point or its handles");
  RNA_def_boolean(ot->srna, "move_point", false, "Move Point", "Move a point or its handles");
  RNA_def_boolean(ot->srna,
                  "cycle_handle_type",
                  false,
                  "Cycle Handle Type",
                  "Cycle between all four handle types");
  RNA_def_float_distance(ot->srna, "radius", 0.01f, 0.0f, FLT_MAX, "Radius", "", 0.0f, 10.0f);
}

static void CURVES_OT_pen(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Curves Pen";
  ot->idname = "CURVES_OT_pen";
  ot->description = "Construct and edit splines";

  /* Callbacks. */
  ot->invoke = curves_pen_invoke;
  ot->modal = curves_pen_modal;

  /* Flags. */
  ot->flag = OPTYPE_UNDO;

  /* Properties. */
  pen_tool_common_props(ot);
}

void ED_operatortypes_curves_pen()
{
  using namespace blender::ed::curves;
  WM_operatortype_append(CURVES_OT_pen);
}

void ED_curves_pentool_modal_keymap(wmKeyConfig *keyconf)
{
  using namespace blender::ed::curves;
  static const EnumPropertyItem modal_items[] = {
      {int(PenModal::MoveHandle),
       "MOVE_HANDLE",
       0,
       "Move Current Handle",
       "Move the current handle of the control point freely"},
      {int(PenModal::MoveEntire),
       "MOVE_ENTIRE",
       0,
       "Move Entire Point",
       "Move the entire point using its handles"},
      {int(PenModal::SnapAngle),
       "SNAP_ANGLE",
       0,
       "Snap Angle",
       "Snap the handle angle to 45 degrees"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Pen Tool Modal Map");

  /* This function is called for each space-type, only needs to add map once. */
  if (keymap && keymap->modal_items) {
    return;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "Pen Tool Modal Map", modal_items);
  WM_modalkeymap_assign(keymap, "CURVES_OT_pen");
}

}  // namespace blender::ed::curves
