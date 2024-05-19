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
  out[2] = mouse[2];
  return true;
}

static void stroke_start_xr(bContext &C,
                         wmOperator &op,
                         const float3 &controller,
                         GreasePencilStrokeOperation &operation)
{
  PaintStroke *paint_stroke = static_cast<PaintStroke *>(op.customdata);
  float2 mouse_xr;
  ARegion *region = CTX_wm_region(&C);

  InputSample start_sample;
  start_sample.controller_position = float3(controller);
  // Debug winrct
  mouse_xr[0] = 2.0f * ((double)controller[0] / region->winx) - 1.0f;
  mouse_xr[1] = (2.0f * ((double)(region->winy - controller[1]) / region->winy)) - 1.0f;
  // start_sample.mouse_position = float2(controller); 
  start_sample.mouse_position = mouse_xr;
  start_sample.pressure = 0.0f; // Bring trigger pressure here?
  start_sample.is_xr = true;

  paint_stroke_set_mode_data(paint_stroke, &operation);
  operation.on_stroke_begin(C, start_sample);
}

static void stroke_update_step(bContext *C,
                               wmOperator * /*op*/,
                               PaintStroke *stroke,
                               PointerRNA *stroke_element)
{
  GreasePencilStrokeOperation *operation = static_cast<GreasePencilStrokeOperation *>(
      paint_stroke_mode_data(stroke));

  InputSample extension_sample;
  RNA_float_get_array(stroke_element, "mouse", extension_sample.mouse_position);
  RNA_float_get_array(stroke_element, "controller", extension_sample.controller_position);
  extension_sample.pressure = RNA_float_get(stroke_element, "pressure");

  if (operation) {
    operation->on_stroke_extended(*C, extension_sample);
  }
}

static void stroke_redraw(const bContext *C, PaintStroke * /*stroke*/, bool /*final*/)
{
  // C will probably not have ARegion, get it somewhere else
  ED_region_tag_redraw(CTX_wm_region(C));
}

static void stroke_done(const bContext *C, PaintStroke *stroke)
{
  GreasePencilStrokeOperation *operation = static_cast<GreasePencilStrokeOperation *>(
      paint_stroke_mode_data(stroke));
  operation->on_stroke_done(*C);
  operation->~GreasePencilStrokeOperation();
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

static GreasePencilStrokeOperation *grease_pencil_brush_stroke_operation(bContext &C)
{
  const Scene &scene = *CTX_data_scene(&C);
  const GpPaint &gp_paint = *scene.toolsettings->gp_paint;
  const Brush &brush = *BKE_paint_brush_for_read(&gp_paint.paint);
  switch (eBrushGPaintTool(brush.gpencil_tool)) {
    case GPAINT_TOOL_DRAW:
      /* FIXME: Somehow store the unique_ptr in the PaintStroke. */
      return greasepencil::new_paint_operation().release();
    case GPAINT_TOOL_ERASE:
      return greasepencil::new_erase_operation().release();
    case GPAINT_TOOL_FILL:
      return nullptr;
    case GPAINT_TOOL_TINT:
      return greasepencil::new_tint_operation().release();
  }
  return nullptr;
}

static bool grease_pencil_brush_stroke_test_start(bContext *C,
                                                  wmOperator *op,
                                                  const float controller[3])
{
  GreasePencilStrokeOperation *operation = grease_pencil_brush_stroke_operation(*C);
  if (operation) {
    stroke_start_xr(*C, *op, float3(controller), *operation);
    return true;
  }
  return false;
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
                                    grease_pencil_brush_stroke_test_start,
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

void GREASE_PENCIL_XR_OT_brush_stroke(wmOperatorType *ot)
{
  using namespace blender::ed::sculpt_paint::greasepencil;
  ot->name = "Grease Pencil XR Draw";
  ot->idname = "GREASE_PENCIL_XR_OT_brush_stroke";
  ot->description = "Draw a new stroke in the active Grease Pencil object";

  ot->poll = grease_pencil_xr_brush_stroke_poll;
  ot->invoke = grease_pencil_xr_brush_stroke_invoke;
  ot->modal = grease_pencil_xr_brush_stroke_modal;
  ot->cancel = grease_pencil_xr_brush_stroke_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  paint_stroke_operator_properties(ot);
}
