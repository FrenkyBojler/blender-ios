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
  bool lock[3];
  RNA_boolean_get_array(op->ptr, "lock", lock);
  bool changed = false;

  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    if (!EDBM_op_callf(em,
                       op,
                       "space_evenly geom=%hvef interpolation=%i factor=%f "
                       "lock_x=%b lock_y=%b lock_z=%b",
                       BM_ELEM_SELECT,
                       interpolation,
                       influence,
                       lock[0],
                       lock[1],
                       lock[2]))
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

static void edbm_space_ui(bContext * /*C*/, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  layout.use_property_split_set(true);

  layout.prop(op->ptr, "influence", UI_ITEM_NONE, IFACE_("Factor"), ICON_NONE);

  ui::Layout &lock_row = layout.row(true, IFACE_("Lock"));
  PropertyRNA *lock_prop = RNA_struct_find_property(op->ptr, "lock");
  lock_row.prop(op->ptr, lock_prop, 0, 0, ui::ITEM_R_TOGGLE, "X", ICON_NONE);
  lock_row.prop(op->ptr, lock_prop, 1, 0, ui::ITEM_R_TOGGLE, "Y", ICON_NONE);
  lock_row.prop(op->ptr, lock_prop, 2, 0, ui::ITEM_R_TOGGLE, "Z", ICON_NONE);

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

  RNA_def_boolean_array(ot->srna, "lock", 3, nullptr, "Lock", "Lock editing of the axis");
  RNA_def_enum(ot->srna,
               "interpolation",
               prop_interpolation_items,
               0,
               "Interpolation",
               "Algorithm used for interpolation");
}

}  // namespace blender
