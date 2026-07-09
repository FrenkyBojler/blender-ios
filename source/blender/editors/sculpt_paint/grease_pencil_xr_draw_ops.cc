/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_context.hh"
#include "BKE_deform.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_object_deform.h"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "BLI_assert.hh"

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

#include "MEM_guardedalloc.h"

#include "grease_pencil/grease_pencil_intern.hh"
#include "paint_intern.hh"

#include <memory>
#include <utility>

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Common Paint Operator Functions
 * \{ */

struct GreasePencilXRPaintStroke final : public PaintStroke {
  GreasePencilXRPaintStroke(bContext *C, wmOperator *op, const int event_type)
      : PaintStroke(C, op, event_type)
  {
  }

  bool get_location(float out[3], const float mouse[2], bool force_original) override;
  bool test_start(wmOperator *op, const float mouse[2]) override;
  void update_step(wmOperator *op, PointerRNA *stroke_element) override;
  void redraw(bool final) override;
  bool test_cancel() override;
  void done(bool is_cancel, bool stroke_started) override;
};

bool GreasePencilXRPaintStroke::get_location(float out[3],
                                             const float mouse[2],
                                             bool /*force_original*/)
{
  out[0] = this->last_controller_position[0];
  out[1] = this->last_controller_position[1];
  out[2] = this->last_controller_position[2];
  return true;
}

static std::unique_ptr<GreasePencilStrokeOperation> get_stroke_operation(bContext &C,
                                                                         wmOperator *op)
{
  const Paint *paint = BKE_paint_get_active_from_context(&C);
  const Brush &brush = *BKE_paint_brush_for_read(paint);
  const PaintMode mode = BKE_paintmode_get_active_from_context(&C);
  const auto stroke_mode = BrushStrokeMode(RNA_enum_get(op->ptr, "mode"));
  const auto brush_switch_mode = BrushSwitchMode(RNA_enum_get(op->ptr, "brush_toggle"));

  if (mode == PaintMode::GPencil) {
    if (eBrushGPaintType(brush.gpencil_brush_type) == GPAINT_BRUSH_TYPE_DRAW &&
        brush_switch_mode == BrushSwitchMode::Erase)
    {
      return greasepencil::new_erase_operation(true);
    }
    switch (eBrushGPaintType(brush.gpencil_brush_type)) {
      case GPAINT_BRUSH_TYPE_DRAW:
        return greasepencil::new_paint_operation();
      case GPAINT_BRUSH_TYPE_ERASE:
        return greasepencil::new_erase_operation();
      case GPAINT_BRUSH_TYPE_FILL:
        /* Fill tool keymap uses the paint operator to draw fill guides. */
        return greasepencil::new_paint_operation(/* do_fill_guides = */ true);
      case GPAINT_BRUSH_TYPE_TINT:
        return greasepencil::new_tint_operation(brush_switch_mode == BrushSwitchMode::Erase);
    }
  }
  else if (mode == PaintMode::SculptGPencil) {
    if (brush_switch_mode == BrushSwitchMode::Smooth) {
      return greasepencil::new_smooth_operation(stroke_mode, true);
    }
    switch (eBrushGPSculptType(brush.gpencil_sculpt_brush_type)) {
      case GPSCULPT_BRUSH_TYPE_SMOOTH:
        return greasepencil::new_smooth_operation(stroke_mode);
      case GPSCULPT_BRUSH_TYPE_THICKNESS:
        return greasepencil::new_thickness_operation(stroke_mode);
      case GPSCULPT_BRUSH_TYPE_STRENGTH:
        return greasepencil::new_strength_operation(stroke_mode);
      case GPSCULPT_BRUSH_TYPE_GRAB:
        return greasepencil::new_grab_operation(stroke_mode);
      case GPSCULPT_BRUSH_TYPE_PUSH:
        return greasepencil::new_push_operation(stroke_mode);
      case GPSCULPT_BRUSH_TYPE_TWIST:
        return greasepencil::new_twist_operation(stroke_mode);
      case GPSCULPT_BRUSH_TYPE_PINCH:
        return greasepencil::new_pinch_operation(stroke_mode);
      case GPSCULPT_BRUSH_TYPE_RANDOMIZE:
        return greasepencil::new_randomize_operation(stroke_mode);
      case GPSCULPT_BRUSH_TYPE_CLONE:
        return greasepencil::new_clone_operation(stroke_mode);
    }
  }
  else if (mode == PaintMode::WeightGPencil) {
    switch (eBrushGPWeightType(brush.gpencil_weight_brush_type)) {
      case GPWEIGHT_BRUSH_TYPE_DRAW:
        return greasepencil::new_weight_paint_draw_operation(stroke_mode);
      case GPWEIGHT_BRUSH_TYPE_BLUR:
        return greasepencil::new_weight_paint_blur_operation();
      case GPWEIGHT_BRUSH_TYPE_AVERAGE:
        return greasepencil::new_weight_paint_average_operation();
      case GPWEIGHT_BRUSH_TYPE_SMEAR:
        return greasepencil::new_weight_paint_smear_operation();
    }
  }
  else if (mode == PaintMode::VertexGPencil) {
    switch (eBrushGPVertexType(brush.gpencil_vertex_brush_type)) {
      case GPVERTEX_BRUSH_TYPE_DRAW:
        return greasepencil::new_vertex_paint_operation(stroke_mode);
      case GPVERTEX_BRUSH_TYPE_BLUR:
        return greasepencil::new_vertex_blur_operation();
      case GPVERTEX_BRUSH_TYPE_AVERAGE:
        return greasepencil::new_vertex_average_operation();
      case GPVERTEX_BRUSH_TYPE_SMEAR:
        return greasepencil::new_vertex_smear_operation();
      case GPVERTEX_BRUSH_TYPE_REPLACE:
        return greasepencil::new_vertex_replace_operation();
      case GPVERTEX_BRUSH_TYPE_TINT:
        BLI_assert_unreachable();
        return nullptr;
    }
  }
  return nullptr;
}

bool GreasePencilXRPaintStroke::test_start(wmOperator * /*op*/, const float /*mouse*/[2])
{
  return true;
}

void GreasePencilXRPaintStroke::update_step(wmOperator *op, PointerRNA *stroke_element)
{
  GreasePencilStrokeOperation *operation = static_cast<GreasePencilStrokeOperation *>(
      mode_data_.get());

  InputSample sample;
  RNA_float_get_array(stroke_element, "mouse", sample.mouse_position);
  RNA_float_get_array(stroke_element, "controller", sample.controller_position);
  sample.pressure = RNA_float_get(stroke_element, "pressure");
  sample.is_xr = true;

  if (!operation) {
    std::unique_ptr<GreasePencilStrokeOperation> new_operation = get_stroke_operation(
        *this->evil_C, op);
    BLI_assert(new_operation != nullptr);
    new_operation->on_stroke_begin(*this->evil_C, sample);
    mode_data_ = std::move(new_operation);
  }
  else {
    operation->on_stroke_extended(*this->evil_C, sample);
  }
}

void GreasePencilXRPaintStroke::redraw(bool /*final*/)
{
  ED_region_tag_redraw(CTX_wm_region(this->evil_C));
}

bool GreasePencilXRPaintStroke::test_cancel()
{
  return false;
}

void GreasePencilXRPaintStroke::done(bool /*is_cancel*/, bool /*stroke_started*/)
{
  GreasePencilStrokeOperation *operation = static_cast<GreasePencilStrokeOperation *>(
      mode_data_.get());
  if (operation != nullptr) {
    operation->on_stroke_done(*this->evil_C);
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
  bool matched = (actiondata->ot == op->type);
  printf("=== GREASE PENCIL DRAW PATH: wm_xr_operator_gpencil_test_event ===\n");
  printf("  -> matched: %d\n", matched);
  fflush(stdout);
  return matched;
}

static wmOperatorStatus grease_pencil_xr_brush_stroke_invoke(bContext *C,
                                                             wmOperator *op,
                                                             const wmEvent *event)
{
  printf("=== GREASE PENCIL DRAW PATH: grease_pencil_xr_brush_stroke_invoke ===\n"); fflush(stdout);
  if (!wm_xr_operator_gpencil_test_event(op, event)) {
    return OPERATOR_PASS_THROUGH;
  }

  const Paint *paint = BKE_paint_get_active_from_context(C);
  const Brush &brush = *BKE_paint_brush_for_read(paint);
  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  const auto brush_switch_mode = BrushSwitchMode(RNA_enum_get(op->ptr, "brush_toggle"));
  const bool use_duplicate_previous_key = mode == PaintMode::GPencil &&
                                          (ELEM(eBrushGPaintType(brush.gpencil_brush_type),
                                                GPAINT_BRUSH_TYPE_ERASE,
                                                GPAINT_BRUSH_TYPE_TINT) ||
                                           (eBrushGPaintType(brush.gpencil_brush_type) ==
                                                GPAINT_BRUSH_TYPE_DRAW &&
                                            brush_switch_mode == BrushSwitchMode::Erase));

  wmOperatorStatus return_value = ed::greasepencil::grease_pencil_draw_operator_invoke(
      C, op, use_duplicate_previous_key);
  if (return_value != OPERATOR_RUNNING_MODAL) {
    return return_value;
  }

  GreasePencilXRPaintStroke *stroke = MEM_new<GreasePencilXRPaintStroke>(
      __func__, C, op, event->type);
  op->customdata = stroke;

  return_value = op->type->modal(C, op, event);
  OPERATOR_RETVAL_CHECK(return_value);
  if (ELEM(return_value, OPERATOR_FINISHED, OPERATOR_CANCELLED)) {
    MEM_delete(stroke);
    op->customdata = nullptr;
    return return_value;
  }

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus grease_pencil_xr_brush_stroke_modal(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent *event)
{
  printf("=== GREASE PENCIL DRAW PATH: grease_pencil_xr_brush_stroke_modal ===\n"); fflush(stdout);
  if (!wm_xr_operator_gpencil_test_event(op, event)) {
    return OPERATOR_PASS_THROUGH;
  }

  GreasePencilXRPaintStroke *stroke = static_cast<GreasePencilXRPaintStroke *>(op->customdata);
  if (stroke == nullptr) {
    return OPERATOR_CANCELLED;
  }

  const wmOperatorStatus retval = stroke->modal(C, op, event);

  if (ELEM(retval, OPERATOR_FINISHED, OPERATOR_CANCELLED)) {
    MEM_delete(stroke);
    op->customdata = nullptr;
  }

  return retval;
}

static void grease_pencil_xr_brush_stroke_cancel(bContext *C, wmOperator *op)
{
  GreasePencilXRPaintStroke *stroke = static_cast<GreasePencilXRPaintStroke *>(op->customdata);
  if (stroke == nullptr) {
    return;
  }

  stroke->cancel(C);
  MEM_delete(stroke);
  op->customdata = nullptr;
}

/** \} */

}  // namespace blender::ed::sculpt_paint

namespace blender {

void GREASE_PENCIL_XR_OT_brush_stroke_xr(wmOperatorType *ot)
{
  using namespace ed::sculpt_paint;
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

}  // namespace blender
