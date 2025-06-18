/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "BLI_math_geom.h"
#include "BLI_math_numbers.hh"
#include "BLI_math_vector.hh"
#include "BLI_vector_set.hh"

#include "DNA_brush_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "ED_curves.hh"
#include "ED_grease_pencil.hh"
#include "ED_view3d.hh"

// rna_Brush_set_size . Maybe add type flag as parameter so we can reuse this function to add the
// strength one too and avoid having two similar functions
int gpencilxr_brush_set_size(bContext *C, int value)
{
  ToolSettings *ts = CTX_data_tool_settings(C);
  Paint *paint = &ts->gp_paint->paint;
  Brush *brush = paint->brush;
  int new_brush_size = brush->size + value;
  new_brush_size = new_brush_size < 1 ? 1 : new_brush_size;
  new_brush_size = new_brush_size > 5000 ? 5000 : new_brush_size;
  /* scale unprojected radius so it stays consistent with brush size */
  BKE_brush_scale_unprojected_radius(&brush->unprojected_radius, new_brush_size, brush->size);
  brush->size = new_brush_size;
  return new_brush_size;
}

float gpencilxr_brush_set_strength(bContext *C, float value)
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

int gpencilxr_brush_get_size(bContext *C)
{
  ToolSettings *ts = CTX_data_tool_settings(C);
  Paint *paint = &ts->gp_paint->paint;
  Brush *brush = paint->brush;
  return brush->size;
}

float gpencilxr_brush_get_strength(bContext *C)
{
  ToolSettings *ts = CTX_data_tool_settings(C);
  Paint *paint = &ts->gp_paint->paint;
  return paint->brush->gpencil_settings->draw_strength;
}
