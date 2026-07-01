/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_object_deform.h"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph_query.hh"

#include "DNA_brush_types.h"
#include "DNA_grease_pencil_types.h"

#include "DNA_scene_types.h"
#include "ED_grease_pencil.hh"
#include "ED_image.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "ANIM_keyframing.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_message.hh"
#include "WM_toolsystem.hh"

#include "curves_sculpt_intern.hh"
#include "grease_pencil_intern.hh"
#include "paint_intern.hh"

namespace blender::ed::sculpt_paint::greasepencil {

/* -------------------------------------------------------------------- */
/** \name Common Paint Operator Functions
 * \{ */

static bool stroke_get_location(bContext * /*C*/,
                                float out[3],
                                const float mouse[3],
                                bool /*force_original*/)
{
  out[0] = mouse[0];
  out[1] = mouse[1];
  out[2] = 0.0f;
  return true;
}

static GreasePencilStrokeOperation *get_stroke_operation(bContext &C, wmOperator *op)
{
  const Paint *paint = BKE_paint_get_active_from_context(&C);
  const Brush &brush = *BKE_paint_brush_for_read(paint);
  const PaintMode mode = BKE_paintmode_get_active_from_context(&C);
  const BrushStrokeMode stroke_mode = BrushStrokeMode(RNA_enum_get(op->ptr, "mode"));

  if (mode == PaintMode::GPencil) {
    /* FIXME: Somehow store the unique_ptr in the PaintStroke. */
    switch (eBrushGPaintTool(brush.gpencil_tool)) {
      case GPAINT_TOOL_DRAW:
        return greasepencil::new_paint_operation().release();
      case GPAINT_TOOL_ERASE:
        return greasepencil::new_erase_operation().release();
      case GPAINT_TOOL_FILL:
        /* Fill tool keymap uses the paint operator as alternative mode. */
        return greasepencil::new_paint_operation().release();
      case GPAINT_TOOL_TINT:
        return greasepencil::new_tint_operation().release();
    }
  }
  else if (mode == PaintMode::SculptGreasePencil) {
    switch (eBrushGPSculptTool(brush.gpencil_sculpt_tool)) {
      case GPSCULPT_TOOL_SMOOTH:
        return greasepencil::new_smooth_operation(stroke_mode).release();
      case GPSCULPT_TOOL_THICKNESS:
        return greasepencil::new_thickness_operation(stroke_mode).release();
      case GPSCULPT_TOOL_STRENGTH:
        return greasepencil::new_strength_operation(stroke_mode).release();
      case GPSCULPT_TOOL_GRAB:
        return greasepencil::new_grab_operation(stroke_mode).release();
      case GPSCULPT_TOOL_PUSH:
        return greasepencil::new_push_operation(stroke_mode).release();
      case GPSCULPT_TOOL_TWIST:
        return greasepencil::new_twist_operation(stroke_mode).release();
      case GPSCULPT_TOOL_PINCH:
        return greasepencil::new_pinch_operation(stroke_mode).release();
      case GPSCULPT_TOOL_RANDOMIZE:
        return greasepencil::new_randomize_operation(stroke_mode).release();
      case GPSCULPT_TOOL_CLONE:
        return greasepencil::new_clone_operation(stroke_mode).release();
    }
  }
  else if (mode == PaintMode::WeightGPencil) {
    switch (eBrushGPWeightTool(brush.gpencil_weight_tool)) {
      case GPWEIGHT_TOOL_DRAW:
        return greasepencil::new_weight_paint_draw_operation(stroke_mode).release();
        break;
      case GPWEIGHT_TOOL_BLUR:
        return greasepencil::new_weight_paint_blur_operation().release();
        break;
      case GPWEIGHT_TOOL_AVERAGE:
        return greasepencil::new_weight_paint_average_operation().release();
        break;
      case GPWEIGHT_TOOL_SMEAR:
        return greasepencil::new_weight_paint_smear_operation().release();
        break;
    }
  }
  return nullptr;
}

static bool stroke_test_start(bContext *C, wmOperator *op, const float mouse[2])
{
  UNUSED_VARS(C, op, mouse);
  return true;
}

static void stroke_update_step(bContext *C,
                               wmOperator * op,
                               PaintStroke *stroke,
                               PointerRNA *stroke_element)
{
  GreasePencilStrokeOperation *operation = static_cast<GreasePencilStrokeOperation *>(
      paint_stroke_mode_data(stroke));

  InputSample sample;
  RNA_float_get_array(stroke_element, "mouse", sample.mouse_position);
  RNA_float_get_array(stroke_element, "controller", sample.controller_position);
  sample.pressure = RNA_float_get(stroke_element, "pressure");
  sample.is_xr = true;

  if (!operation) {
    GreasePencilStrokeOperation *new_operation = get_stroke_operation(*C, op);
    BLI_assert(new_operation != nullptr);
    paint_stroke_set_mode_data(stroke, new_operation);
    new_operation->on_stroke_begin(*C, sample);
  }
  else {
    operation->on_stroke_extended(*C, sample);
  }
}

static void stroke_redraw(const bContext *C, PaintStroke * /*stroke*/, bool /*final*/)
{
  ED_region_tag_redraw(CTX_wm_region(C));
}

static void stroke_done(const bContext *C, PaintStroke *stroke)
{
  GreasePencilStrokeOperation *operation = static_cast<GreasePencilStrokeOperation *>(
      paint_stroke_mode_data(stroke));
  if (operation != nullptr) {
    operation->on_stroke_done(*C);
    operation->~GreasePencilStrokeOperation();
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush Stroke Operator
 * \{ */

static bool grease_pencil_xr_brush_stroke_poll(bContext *C)
{
  wmWindowManager *wm = CTX_wm_manager(C);

  if (!WM_xr_session_is_ready(&wm->xr)) {
    return false;
  }
  if (!ed::greasepencil::grease_pencil_painting_poll(C)) {
    return false;
  }
  if (!WM_toolsystem_active_tool_is_brush(C)) {
    return false;
  }
  return true;
}

static bool wm_xr_operator_gpencil_test_event(const wmOperator *op, const wmEvent *event)
{
  if (event->type != EVT_XR_ACTION) {
    return false;
  }

  BLI_assert(event->custom == EVT_DATA_XR);
  BLI_assert(event->customdata);

  wmXrActionData *actiondata = static_cast<wmXrActionData *>(event->customdata);
  return actiondata->ot == op->type;
}

static int grease_pencil_xr_brush_stroke_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm_xr_operator_gpencil_test_event(op, event)) {
    return OPERATOR_PASS_THROUGH;
  }

  int return_value = ed::greasepencil::grease_pencil_draw_operator_invoke(C, op);
  if (return_value != OPERATOR_RUNNING_MODAL) {
    return return_value;
  }

  op->customdata = paint_stroke_new(C,
                                    op,
                                    stroke_get_location,
                                    stroke_test_start,
                                    stroke_update_step,
                                    stroke_redraw,
                                    stroke_done,
                                    event->type);

  return_value = op->type->modal(C, op, event);
  if (return_value == OPERATOR_FINISHED) {
    return OPERATOR_FINISHED;
  }

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static int grease_pencil_xr_brush_stroke_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm_xr_operator_gpencil_test_event(op, event)) {
    return OPERATOR_PASS_THROUGH;
  }
  return paint_stroke_modal(C, op, event, reinterpret_cast<PaintStroke **>(&op->customdata));
}

static void grease_pencil_xr_brush_stroke_cancel(bContext *C, wmOperator *op)
{
  paint_stroke_cancel(C, op, static_cast<PaintStroke *>(op->customdata));
}

/** \} */

}  // namespace blender::ed::sculpt_paint

void GREASE_PENCIL_XR_OT_brush_stroke_xr(wmOperatorType *ot)
{
  using namespace blender::ed::sculpt_paint::greasepencil;
  ot->name = "Grease Pencil XR Draw";
  ot->idname = "GREASE_PENCIL_XR_OT_brush_stroke_xr";
  ot->description = "Draw a new XR stroke in the active Grease Pencil object";

  ot->poll = grease_pencil_xr_brush_stroke_poll;
  ot->invoke = grease_pencil_xr_brush_stroke_invoke;
  ot->modal = grease_pencil_xr_brush_stroke_modal;
  ot->cancel = grease_pencil_xr_brush_stroke_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  paint_stroke_operator_properties(ot);
}
