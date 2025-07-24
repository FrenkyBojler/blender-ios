/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "BKE_brush.hh"
#include "BKE_context.hh"

#include "DNA_brush_types.h"
#include "DNA_view3d_types.h"

#include "ED_grease_pencil.hh"
#include "ED_view3d.hh"

float ED_grease_pencil_xr_brush_strength_set(bContext *C, float value)
{
  ToolSettings *ts = CTX_data_tool_settings(C);
  Paint *paint = &ts->gp_paint->paint;
  Brush *brush = paint->brush;
  BrushGpencilSettings *brush_settings = brush->gpencil_settings;
  float new_brush_strength = brush_settings->draw_strength + value;
  float new_brush_alpha = brush->alpha + value;
  new_brush_strength = new_brush_strength < 0 ? 0.1f : new_brush_strength;
  new_brush_strength = new_brush_strength > 1 ? 1 : new_brush_strength;
  new_brush_alpha = new_brush_alpha < 0 ? 0.1f : new_brush_alpha;
  new_brush_alpha = new_brush_alpha > 1 ? 1 : new_brush_alpha;
  brush->alpha = new_brush_alpha;
  brush_settings->draw_strength = new_brush_strength;
  return new_brush_strength;
}

int ED_grease_pencil_xr_brush_size_get(bContext *C)
{
  ToolSettings *ts = CTX_data_tool_settings(C);
  Paint *paint = &ts->gp_paint->paint;
  Brush *brush = paint->brush;
  return brush->size;
}

float ED_grease_pencil_xr_brush_strength_get(bContext *C)
{
  ToolSettings *ts = CTX_data_tool_settings(C);
  Paint *paint = &ts->gp_paint->paint;
  return paint->brush->gpencil_settings->draw_strength;
}
