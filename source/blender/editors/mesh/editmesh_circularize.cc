/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 */

#include "BKE_context.hh"
#include "BKE_editmesh.hh"
#include "BKE_layer.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "ED_mesh.hh"
#include "ED_screen.hh"

#include "mesh_intern.hh" /* own include */

static const EnumPropertyItem prop_fit_method_items[] = {
    {0, "BEST", 0, "Best Fit", "Non-linear least squares"},
    {1, "INSIDE", 0, "Fit Inside", "Only move vertices towards the center"},
    {0, nullptr},
};

static wmOperatorStatus edbm_circularize_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const float influence = RNA_float_get(op->ptr, "influence");
  const bool flatten = RNA_boolean_get(op->ptr, "flatten");
  const bool regular = RNA_boolean_get(op->ptr, "regular");
  const int fit_method = RNA_enum_get(op->ptr, "fit_method");
  const float custom_radius = RNA_float_get(op->ptr, "custom_radius");
  const float angle = RNA_float_get(op->ptr, "angle");

  const blender::Vector<Object *> objects =
      BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
          scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;

    if (bm->totvert < 3) {
      continue;
    }

    BMO_op_callf(bm,
                 BMO_FLAG_DEFAULTS,
                 "circularize geom=%hvef influence=%f flatten=%b regular=%b fit_method=%i "
                 "custom_radius=%f angle=%f",
                 BM_ELEM_SELECT,
                 influence,
                 flatten,
                 regular,
                 fit_method,
                 custom_radius,
                 angle);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(static_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

void MESH_OT_circularize(wmOperatorType *ot)
{
  PropertyRNA *prop;
  /* identifiers */
  ot->name = "Circularize";
  ot->description = "Shape selected geometry into a circle";
  ot->idname = "MESH_OT_circularize";

  /* API callbacks */
  ot->exec = edbm_circularize_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_float_factor(
      ot->srna, "influence", 1.0f, 0.0f, 1.0f, "Influence", "Force of the tool", 0.0f, 1.0f);

  RNA_def_boolean(ot->srna,
                  "flatten",
                  true,
                  "Flatten",
                  "Flatten the circle, instead of projecting it on the mesh");
  RNA_def_boolean(ot->srna,
                  "regular",
                  true,
                  "Regular",
                  "Distribute vertices at constant distances along the circle");
  RNA_def_enum(ot->srna,
               "fit_method",
               prop_fit_method_items,
               0,
               "Fit Method",
               "Method used for fitting a circle to the vertices");
  RNA_def_float(ot->srna,
                "custom_radius",
                0.0f,
                0.0f,
                10000.0f,
                "Radius",
                "Custom radius for circle",
                0.0f,
                1000.0f);

  prop = RNA_def_float(ot->srna,
                       "angle",
                       0.0f,
                       -M_PI * 2.0f,
                       M_PI * 2.0f,
                       "Angle",
                       "Rotate the circle",
                       -M_PI * 2.0f,
                       M_PI * 2.0f);
  RNA_def_property_subtype(prop, PROP_ANGLE);
}
