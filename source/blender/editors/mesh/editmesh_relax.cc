/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 */

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_layer.hh"
#include "BLI_listbase.h"
#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "ED_mesh.hh"
#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "mesh_intern.hh" /* own include */

namespace blender {

static const EnumPropertyItem prop_interpolation_items[] = {
    {0, "CUBIC", 0, "Cubic", "Natural cubic spline, smooth results"},
    {1, "LINEAR", 0, "Linear", "Simple and fast linear algorithm"},
    {0, nullptr},
};

static const EnumPropertyItem prop_iterations_items[] = {
    {1, "1", 0, "1", "One"},
    {3, "3", 0, "3", "Three"},
    {5, "5", 0, "5", "Five"},
    {10, "10", 0, "10", "Ten"},
    {25, "25", 0, "25", "Twenty-five"},
    {0, nullptr},
};

static wmOperatorStatus edbm_relax_exec(bContext *C, wmOperator *op)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  const int interpolation = RNA_enum_get(op->ptr, "interpolation");
  const int iterations = RNA_enum_get(op->ptr, "iterations");
  const bool regular = RNA_boolean_get(op->ptr, "regular");
  bool changed = false;

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (!EDBM_op_callf(em,
                       op,
                       "relax geom=%hvef interpolation=%i iterations=%i regular=%b",
                       BM_ELEM_SELECT,
                       interpolation,
                       iterations,
                       regular))
    {
      continue;
    }

    changed = true;
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    EDBM_update(id_cast<Mesh *>(obedit->data), &params);
  }

  return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static void edbm_relax_ui(bContext * /*C*/, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  layout.use_property_split_set(true);
  layout.prop(op->ptr, "iterations", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "regular", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "interpolation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

void MESH_OT_relax(wmOperatorType *ot)
{
  ot->name = "Relax";
  ot->description = "Relax the loop, so it is smoother";
  ot->idname = "MESH_OT_relax";

  ot->exec = edbm_relax_exec;
  ot->poll = ED_operator_editmesh;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->ui = edbm_relax_ui;

  RNA_def_enum(ot->srna,
               "interpolation",
               prop_interpolation_items,
               0,
               "Interpolation",
               "Algorithm used for interpolation");
  RNA_def_enum(ot->srna,
               "iterations",
               prop_iterations_items,
               1,
               "Iterations",
               "Number of times the loop is relaxed");
  RNA_def_boolean(ot->srna,
                  "regular",
                  true,
                  "Space evenly",
                  "Distribute vertices at constant distances along the loop");
}

}  // namespace blender
