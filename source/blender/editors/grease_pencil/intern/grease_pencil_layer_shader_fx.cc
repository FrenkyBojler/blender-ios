/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "DNA_grease_pencil_types.h"
#include "DNA_object_types.h"
#include "DNA_shader_fx_types.h"

#include "BLI_listbase.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_utildefines.hh"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_report.hh"
#include "BKE_shader_fx.hh"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::greasepencil {

static GreasePencil *get_grease_pencil(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_GREASE_PENCIL) {
    return nullptr;
  }
  return id_cast<GreasePencil *>(ob->data);
}

static bke::greasepencil::Layer *get_active_layer(bContext *C)
{
  GreasePencil *grease_pencil = get_grease_pencil(C);
  if (!grease_pencil) {
    return nullptr;
  }
  return grease_pencil->get_active_layer();
}

static bool layer_shaderfx_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_GREASE_PENCIL) {
    return false;
  }
  return get_active_layer(C) != nullptr;
}

/* -------------------------------------------------------------------- */
/** \name Add Layer Effect Operator
 * \{ */

static wmOperatorStatus layer_shaderfx_add_exec(bContext *C, wmOperator *op)
{
  GreasePencil *grease_pencil = get_grease_pencil(C);
  bke::greasepencil::Layer *layer = get_active_layer(C);
  if (!grease_pencil || !layer) {
    return OPERATOR_CANCELLED;
  }

  const int type = RNA_enum_get(op->ptr, "type");
  const ShaderFxTypeInfo *fxi = BKE_shaderfx_get_info(ShaderFxType(type));

  if (fxi->flags & eShaderFxTypeFlag_Single) {
    for (ShaderFxData &existing : layer->shader_fx) {
      if (existing.type == type) {
        BKE_report(op->reports, RPT_WARNING, "Only one effect of this type is allowed");
        return OPERATOR_CANCELLED;
      }
    }
  }

  ShaderFxData *fx = BKE_shaderfx_new(ShaderFxType(type));
  BKE_shaderfx_unique_name(&layer->shader_fx, fx);
  BLI_addtail(&layer->shader_fx, fx);

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, grease_pencil);

  return OPERATOR_FINISHED;
}

void GREASE_PENCIL_OT_layer_shaderfx_add(wmOperatorType *ot)
{
  ot->name = "Add Layer Effect";
  ot->description = "Add a visual effect to the active Grease Pencil layer";
  ot->idname = "GREASE_PENCIL_OT_layer_shaderfx_add";

  ot->invoke = WM_menu_invoke;
  ot->exec = layer_shaderfx_add_exec;
  ot->poll = layer_shaderfx_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(
      ot->srna, "type", rna_enum_object_shaderfx_type_items, eShaderFxType_Blur, "Type", "");
  RNA_def_property_translation_context(ot->prop, BLT_I18NCONTEXT_ID_ID);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Remove Layer Effect Operator
 * \{ */

static wmOperatorStatus layer_shaderfx_remove_exec(bContext *C, wmOperator *op)
{
  GreasePencil *grease_pencil = get_grease_pencil(C);
  bke::greasepencil::Layer *layer = get_active_layer(C);
  if (!grease_pencil || !layer) {
    return OPERATOR_CANCELLED;
  }

  char fx_name[MAX_NAME];
  RNA_string_get(op->ptr, "shaderfx", fx_name);

  ShaderFxData *fx = static_cast<ShaderFxData *>(
      BLI_findstring(&layer->shader_fx, fx_name, offsetof(ShaderFxData, name)));
  if (!fx) {
    BKE_reportf(op->reports, RPT_ERROR, "Effect '%s' not found on active layer", fx_name);
    return OPERATOR_CANCELLED;
  }

  BLI_remlink(&layer->shader_fx, fx);
  BKE_shaderfx_free(fx);

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, grease_pencil);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus layer_shaderfx_remove_invoke(bContext *C,
                                                     wmOperator *op,
                                                     const wmEvent * /*event*/)
{
  /* Try to get the effect name from context (panel custom data). */
  if (!RNA_struct_property_is_set(op->ptr, "shaderfx")) {
    bke::greasepencil::Layer *layer = get_active_layer(C);
    if (layer && layer->shader_fx.last) {
      ShaderFxData *fx = static_cast<ShaderFxData *>(layer->shader_fx.last);
      RNA_string_set(op->ptr, "shaderfx", fx->name);
    }
  }
  return layer_shaderfx_remove_exec(C, op);
}

void GREASE_PENCIL_OT_layer_shaderfx_remove(wmOperatorType *ot)
{
  ot->name = "Remove Layer Effect";
  ot->description = "Remove a visual effect from the active Grease Pencil layer";
  ot->idname = "GREASE_PENCIL_OT_layer_shaderfx_remove";

  ot->invoke = layer_shaderfx_remove_invoke;
  ot->exec = layer_shaderfx_remove_exec;
  ot->poll = layer_shaderfx_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  PropertyRNA *prop = RNA_def_string(
      ot->srna, "shaderfx", nullptr, MAX_NAME, "Effect", "Name of the effect to remove");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Move Layer Effect Operator
 * \{ */

static wmOperatorStatus layer_shaderfx_move_exec(bContext *C, wmOperator *op)
{
  GreasePencil *grease_pencil = get_grease_pencil(C);
  bke::greasepencil::Layer *layer = get_active_layer(C);
  if (!grease_pencil || !layer) {
    return OPERATOR_CANCELLED;
  }

  char fx_name[MAX_NAME];
  RNA_string_get(op->ptr, "shaderfx", fx_name);

  ShaderFxData *fx = static_cast<ShaderFxData *>(
      BLI_findstring(&layer->shader_fx, fx_name, offsetof(ShaderFxData, name)));
  if (!fx) {
    return OPERATOR_CANCELLED;
  }

  const int direction = RNA_enum_get(op->ptr, "direction");
  if (direction == 1) {
    /* Move up. */
    if (fx->prev) {
      BLI_remlink(&layer->shader_fx, fx);
      BLI_insertlinkbefore(&layer->shader_fx, fx->prev, fx);
    }
  }
  else {
    /* Move down. */
    if (fx->next) {
      BLI_remlink(&layer->shader_fx, fx);
      BLI_insertlinkafter(&layer->shader_fx, fx->next, fx);
    }
  }

  DEG_id_tag_update(&grease_pencil->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, grease_pencil);

  return OPERATOR_FINISHED;
}

void GREASE_PENCIL_OT_layer_shaderfx_move(wmOperatorType *ot)
{
  static const EnumPropertyItem direction_items[] = {
      {1, "UP", 0, "Up", ""},
      {-1, "DOWN", 0, "Down", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Move Layer Effect";
  ot->description = "Move a visual effect up or down in the active layer's stack";
  ot->idname = "GREASE_PENCIL_OT_layer_shaderfx_move";

  ot->exec = layer_shaderfx_move_exec;
  ot->poll = layer_shaderfx_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  PropertyRNA *prop = RNA_def_string(
      ot->srna, "shaderfx", nullptr, MAX_NAME, "Effect", "Name of the effect to move");
  RNA_def_property_flag(prop, PROP_HIDDEN);

  RNA_def_enum(ot->srna, "direction", direction_items, 1, "Direction", "");
}

/** \} */

}  // namespace blender::ed::greasepencil

namespace blender {

void ED_operatortypes_grease_pencil_layer_shader_fx()
{
  using namespace ed::greasepencil;
  WM_operatortype_append(GREASE_PENCIL_OT_layer_shaderfx_add);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_shaderfx_remove);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_shaderfx_move);
}

}  // namespace blender
