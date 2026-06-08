/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 *
 * Editor operator for edge flow.
 */

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_layer.hh"

#include "WM_types.hh"

#include "ED_mesh.hh"
#include "ED_screen.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

#include "mesh_intern.hh"

namespace blender {

static wmOperatorStatus edbm_edge_flow_exec(bContext *C, wmOperator *op)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  const Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  bool changed = false;

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);

    if (em->bm->totedgesel == 0) {
      continue;
    }

    const int mode = RNA_enum_get(op->ptr, "mode");
    const float mix = RNA_float_get(op->ptr, "mix");
    const bool space_evenly = RNA_boolean_get(op->ptr, "space_evenly");

    if (!EDBM_op_callf(em,
                       op,
                       "edge_flow edges=%he mode=%i mix=%f space_evenly=%b",
                       BM_ELEM_SELECT,
                       mode,
                       mix,
                       space_evenly))
    {
      continue;
    }

    changed = true;
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = false;
    EDBM_update(id_cast<Mesh *>(obedit->data), &params);
  }

  return changed ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
}

static const EnumPropertyItem mode_items[] = {
    {EDGE_FLOW_LINEAR, "LINEAR", 0, "Linear", "Straighten the loop between endpoints"},
    {EDGE_FLOW_FLOW, "FLOW", 0, "Flow", "Adjust loop to match surrounding geometry"},
    {0, nullptr, 0, nullptr, nullptr},
};

void MESH_OT_edge_flow(wmOperatorType *ot)
{
  ot->name = "Edge Flow";
  ot->idname = "MESH_OT_edge_flow";
  ot->description = "Set edge flow for the selected edges";

  ot->exec = edbm_edge_flow_exec;
  ot->poll = ED_operator_editmesh;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "mode", mode_items, EDGE_FLOW_LINEAR, "Mode", "");

  RNA_def_float(ot->srna,
                "mix",
                1.0f,
                0.0f,
                1.0f,
                "Mix",
                "Mix between original and edge flow position",
                0.0f,
                1.0f);

  RNA_def_boolean(
      ot->srna, "space_evenly", false, "Space Evenly", "Space edges evenly along the loop");
}

}  // namespace blender
