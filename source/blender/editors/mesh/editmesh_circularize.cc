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
#include "BLI_math_numbers.hh"
#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "ED_mesh.hh"
#include "ED_screen.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "mesh_intern.hh" /* own include */

namespace blender {

static const EnumPropertyItem prop_fit_method_items[] = {
    {0, "BEST", 0, "Best Fit", "Non-linear least squares"},
    {1, "INSIDE", 0, "Fit Inside", "Only move vertices towards the center"},
    {0, nullptr},
};

static wmOperatorStatus edbm_circularize_exec(bContext *C, wmOperator *op)
{
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  const float factor = RNA_float_get(op->ptr, "factor");
  const bool flatten = RNA_boolean_get(op->ptr, "flatten");
  const bool regular = RNA_boolean_get(op->ptr, "regular");
  const int fit_method = RNA_enum_get(op->ptr, "fit_method");
  float custom_radius = RNA_float_get(op->ptr, "custom_radius");
  if (!RNA_boolean_get(op->ptr, "use_custom_radius")) {
    custom_radius = 0.0f;
  }
  const float angle = RNA_float_get(op->ptr, "angle");
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

    bool check_mirror = false;
    for (ModifierData &md : obedit->modifiers) {
      if (md.type == eModifierType_Mirror && (md.mode & eModifierMode_Realtime)) {
        MirrorModifierData *mmd = reinterpret_cast<MirrorModifierData *>(&md);
        /* We should only enable check_mirror when merge is on.
         * It doesn't make sense to create a half circle when merge is off. */
        if (!(mmd->flag & MOD_MIR_NO_MERGE)) {
          check_mirror = true;
          break;
        }
      }
    }

    BMO_op_callf(bm,
                 BMO_FLAG_DEFAULTS,
                 "circularize geom=%hvef factor=%f flatten=%b regular=%b fit_method=%i "
                 "custom_radius=%f angle=%f lock_x=%b lock_y=%b lock_z=%b check_mirror=%b",
                 BM_ELEM_SELECT,
                 factor,
                 flatten,
                 regular,
                 fit_method,
                 custom_radius,
                 angle,
                 lock_x,
                 lock_y,
                 lock_z,
                 check_mirror);

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(id_cast<Mesh *>(obedit->data), &params);
  }

  return OPERATOR_FINISHED;
}

static void edbm_circularize_ui(bContext * /*C*/, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  layout.use_property_split_set(true);

  layout.prop(op->ptr, "factor", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "fit_method", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "angle", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &row = layout.row(true);
  row.prop(op->ptr, "use_custom_radius", UI_ITEM_NONE, "", ICON_NONE);
  ui::Layout &sub = row.row(true);
  sub.active_set(RNA_boolean_get(op->ptr, "use_custom_radius"));
  sub.prop(op->ptr, "custom_radius", UI_ITEM_NONE, "Radius", ICON_NONE);

  layout.prop(op->ptr, "regular", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(op->ptr, "flatten", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout &lock_row = layout.row(true, IFACE_("Lock"));
  lock_row.prop(op->ptr, "lock_x", ui::ITEM_R_TOGGLE, "X", ICON_NONE);
  lock_row.prop(op->ptr, "lock_y", ui::ITEM_R_TOGGLE, "Y", ICON_NONE);
  lock_row.prop(op->ptr, "lock_z", ui::ITEM_R_TOGGLE, "Z", ICON_NONE);
}

void MESH_OT_circularize(wmOperatorType *ot)
{
  PropertyRNA *prop;
  /* identifiers */
  ot->name = "To circle";
  ot->description = "Shape selected geometry into a circle";
  ot->idname = "MESH_OT_circularize";

  /* API callbacks */
  ot->exec = edbm_circularize_exec;
  ot->poll = ED_operator_editmesh;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->ui = edbm_circularize_ui;

  prop = RNA_def_float_factor(
      ot->srna, "factor", 1.0f, 0.0f, 1.0f, "Factor", "Force of the tool", 0.0f, 1.0f);
  RNA_def_enum(ot->srna,
               "fit_method",
               prop_fit_method_items,
               0,
               "Method",
               "Method used for fitting a circle to the vertices");
  prop = RNA_def_float(ot->srna,
                       "angle",
                       0.0f,
                       -math::numbers::pi * 2.0f,
                       math::numbers::pi * 2.0f,
                       "Rotation",
                       "Rotate the circle",
                       -math::numbers::pi * 2.0f,
                       math::numbers::pi * 2.0f);
  RNA_def_property_subtype(prop, PROP_ANGLE);
  RNA_def_boolean(
      ot->srna, "use_custom_radius", false, "Use Custom Radius", "Enable custom radius");
  RNA_def_float(ot->srna,
                "custom_radius",
                0.0f,
                0.0f,
                10000.0f,
                "Radius",
                "Custom radius for circle",
                0.0f,
                1000.0f);
  RNA_def_boolean(ot->srna,
                  "regular",
                  true,
                  "Space Evenly",
                  "Distribute vertices at constant distances along the circle");
  RNA_def_boolean(ot->srna,
                  "flatten",
                  true,
                  "Flatten",
                  "Flatten the circle, instead of projecting it on the mesh");
  RNA_def_boolean(ot->srna, "lock_x", false, "Lock X", "Lock editing of the X-coordinate");
  RNA_def_boolean(ot->srna, "lock_y", false, "Lock Y", "Lock editing of the Y-coordinate");
  RNA_def_boolean(ot->srna, "lock_z", false, "Lock Z", "Lock editing of the Z-coordinate");
}

}  // namespace blender
