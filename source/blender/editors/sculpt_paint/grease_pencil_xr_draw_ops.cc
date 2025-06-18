/** TODO implement carefully it doesn't work with 4.3 alpha version we made */
/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "ANIM_keyframing.hh"

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_crazyspace.hh"
#include "BKE_curves.hh"
#include "BKE_deform.hh"
#include "BKE_geometry_set.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_material.hh"
#include "BKE_object_deform.h"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLI_array_utils.hh"
#include "BLI_assert.h"
#include "BLI_bounds.hh"
#include "BLI_color.hh"
#include "BLI_index_mask.hh"
#include "BLI_kdopbvh.hh"
#include "BLI_kdtree.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_rect.h"

#include "DNA_brush_enums.h"
#include "DNA_brush_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "DEG_depsgraph_query.hh"

#include "GEO_join_geometries.hh"
#include "GEO_smooth_curves.hh"

#include "ED_grease_pencil.hh"
#include "ED_image.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_view3d.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "grease_pencil_intern.hh"
#include "paint_intern.hh"
#include "wm_event_types.hh"

#include <algorithm>
#include <fmt/format.h>
#include <optional>

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
  out[2] = 0;
  return true;
}

static std::unique_ptr<GreasePencilStrokeOperation> get_stroke_operation(bContext &C,
                                                                         wmOperator *op)
{
  const Paint *paint = BKE_paint_get_active_from_context(&C);
  const Brush &brush = *BKE_paint_brush_for_read(paint);
  const PaintMode mode = BKE_paintmode_get_active_from_context(&C);
  const BrushStrokeMode stroke_mode = BrushStrokeMode(RNA_enum_get(op->ptr, "mode"));

  if (mode == PaintMode::GPencil) {
    if (eBrushGPaintType(brush.gpencil_brush_type) == GPAINT_BRUSH_TYPE_DRAW &&
        stroke_mode == BRUSH_STROKE_ERASE)
    {
      /* Special case: We're using the draw tool but with the eraser mode, so create an erase
       * operation. */
      return greasepencil::new_erase_operation(true);
    }
    /* FIXME: Somehow store the unique_ptr in the PaintStroke. */
    switch (eBrushGPaintType(brush.gpencil_brush_type)) {
      case GPAINT_BRUSH_TYPE_DRAW:
        return greasepencil::new_paint_operation();
      case GPAINT_BRUSH_TYPE_ERASE:
        return greasepencil::new_erase_operation();
      case GPAINT_BRUSH_TYPE_FILL:
        /* Fill tool keymap uses the paint operator as alternative mode. */
        return greasepencil::new_paint_operation(true);
      case GPAINT_BRUSH_TYPE_TINT:
        return greasepencil::new_tint_operation();
    }
  }
  else if (mode == PaintMode::SculptGPencil) {

    if (stroke_mode == BRUSH_STROKE_SMOOTH) {
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
        /* Unused. */
        BLI_assert_unreachable();
        return nullptr;
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
                               wmOperator *op,
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
    std::unique_ptr<GreasePencilStrokeOperation> new_operation = get_stroke_operation(*C, op);
    BLI_assert(new_operation != nullptr);
    new_operation->on_stroke_begin(*C, sample);
    paint_stroke_set_mode_data(stroke, std::move(new_operation));
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
//   wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm_xr_operator_gpencil_test_event(op, event)) {
    return OPERATOR_PASS_THROUGH;
  }

  const bool use_duplicate_previous_key = [&]() -> bool {
    const Paint *paint = BKE_paint_get_active_from_context(C);
    const Brush &brush = *BKE_paint_brush_for_read(paint);
    const PaintMode mode = BKE_paintmode_get_active_from_context(C);
    const BrushStrokeMode stroke_mode = BrushStrokeMode(RNA_enum_get(op->ptr, "mode"));

    if (mode == PaintMode::GPencil) {
      /* For the eraser and tint tool, we don't want auto-key to create an empty keyframe, so we
       * duplicate the previous frame. */
      if (ELEM(eBrushGPaintType(brush.gpencil_brush_type),
               GPAINT_BRUSH_TYPE_ERASE,
               GPAINT_BRUSH_TYPE_TINT))
      {
        return true;
      }
      /* Same for the temporary eraser when using the draw tool. */
      if (eBrushGPaintType(brush.gpencil_brush_type) == GPAINT_BRUSH_TYPE_DRAW &&
          stroke_mode == BRUSH_STROKE_ERASE)
      {
        return true;
      }
    }
    return false;
  }();

  int return_value = ed::greasepencil::grease_pencil_draw_operator_invoke(
      C, op, use_duplicate_previous_key);
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

}  // namespace blender::ed::sculpt_paint::greasepencil

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
