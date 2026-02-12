/* SPDX-FileCopyrightText: 2025 Blender Authors
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
    {1, "LINEAR", 0, "Linear", "Vertices are projected on existing edges"},
    {0, nullptr},
};

static wmOperatorStatus edbm_space_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const float influence = RNA_float_get(op->ptr, "influence");
  const int interpolation = RNA_enum_get(op->ptr, "interpolation");
  const bool use_parallel = RNA_boolean_get(op->ptr, "use_parallel");
  const bool lock_x = RNA_boolean_get(op->ptr, "lock_x");
  const bool lock_y = RNA_boolean_get(op->ptr, "lock_y");
  const bool lock_z = RNA_boolean_get(op->ptr, "lock_z");

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if (bm->totvert < 3) {
      continue;
    }

    BMO_op_callf(
        bm,
        BMO_FLAG_DEFAULTS,
        "space_evenly geom=%hvef interpolation=%i input=%i factor=%f lock_x=%b lock_y=%b lock_z=%b",
        BM_ELEM_SELECT,
        interpolation,
        use_parallel ? 1 : 0,
        influence,
        lock_x,
        lock_y,
        lock_z);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(id_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

static void edbm_space_ui(bContext * /*C*/, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  layout.use_property_split_set(true);

  layout.prop(op->ptr, "influence", UI_ITEM_NONE, IFACE_("Factor"), ICON_NONE);
  layout.prop(op->ptr, "use_parallel", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &lock_row = layout.row(true, IFACE_("Lock"));
  lock_row.prop(op->ptr, "lock_x", ui::ITEM_R_TOGGLE, "X", ICON_NONE);
  lock_row.prop(op->ptr, "lock_y", ui::ITEM_R_TOGGLE, "Y", ICON_NONE);
  lock_row.prop(op->ptr, "lock_z", ui::ITEM_R_TOGGLE, "Z", ICON_NONE);

  layout.prop(op->ptr, "interpolation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

void MESH_OT_space_evenly(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Space Evenly";
  ot->description = "Space the vertices in a regular distribution on the loop";
  ot->idname = "MESH_OT_space_evenly";

  /* API callbacks */
  ot->exec = edbm_space_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->ui = edbm_space_ui;

  RNA_def_float_factor(
      ot->srna, "influence", 1.0f, 0.0f, 1.0f, "Influence", "Force of the tool", 0.0f, 1.0f);

  RNA_def_boolean(ot->srna,
                  "use_parallel",
                  true,
                  "Parallel Loops",
                  "Also use non-selected parallel loops as input");

  RNA_def_boolean(ot->srna, "lock_x", false, "Lock X", "Lock editing of the X-coordinate");
  RNA_def_boolean(ot->srna, "lock_y", false, "Lock Y", "Lock editing of the Y-coordinate");
  RNA_def_boolean(ot->srna, "lock_z", false, "Lock Z", "Lock editing of the Z-coordinate");

  RNA_def_enum(ot->srna,
               "interpolation",
               prop_interpolation_items,
               0,
               "Interpolation",
               "Algorithm used for interpolation");
}

}  // namespace blender
