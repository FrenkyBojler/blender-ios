/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Share between `interface/templates/` files.
 */

#pragma once

#include "BKE_context.hh"
#include "MEM_guardedalloc.h"

#include "BKE_colortools.hh"
#include "BLI_rect.h"
#include "ED_screen.hh"
#include "ED_undo.hh"

#include "RNA_access.hh"
#include "RNA_types.hh"

#include "BLT_translation.hh"
#include "UI_interface.hh"

struct bContext;

/* Zoom-related constants for CurveMapping. */
#define CURVE_ZOOM_MAX (1.0f / 25.0f)
#define CURVE_ZOOM_IN_SCALE_FACTOR 0.1154f
#define CURVE_ZOOM_OUT_SCALE_FACTOR 0.15f
/* default bounds for curvemap*/
#define DEFAULT_BOUNDS_MIN -1000.0f
#define DEFAULT_BOUNDS_MAX 1000.0f

/* Clipping options float input limits*/
// Frequency range for the X-axis
#define MIN_FREQUENCY_X 0.0f
#define MAX_FREQUENCY_X 20000.0f

// Decibel range for the Y-axis
#define MIN_DECIBEL_Y -100.0f
#define MAX_DECIBEL_Y 100.0f

#define ERROR_LIBDATA_MESSAGE N_("Can't edit external library data")

/* Defines for templateID/TemplateSearch. */
#define TEMPLATE_SEARCH_TEXTBUT_MIN_WIDTH (UI_UNIT_X * 4)
#define TEMPLATE_SEARCH_TEXTBUT_HEIGHT UI_UNIT_Y

struct RNAUpdateCb {
  PointerRNA ptr = {};
  PropertyRNA *prop;
};

static inline void rna_update_cb(bContext &C, const RNAUpdateCb &cb)
{
  /* we call update here on the pointer property, this way the
   * owner of the curve mapping can still define its own update
   * and notifier, even if the CurveMapping struct is shared. */
  RNA_property_update(&C, &const_cast<PointerRNA &>(cb.ptr), cb.prop);
}

static inline void rna_update_cb(bContext *C, void *arg_cb, void * /*arg*/)
{
  RNAUpdateCb *cb = (RNAUpdateCb *)arg_cb;
  rna_update_cb(*C, *cb);
}

/* `interface_template.cc` */
int template_search_textbut_width(PointerRNA *ptr, PropertyRNA *name_prop);
int template_search_textbut_height();
void template_add_button_search_menu(const bContext *C,
                                     uiLayout *layout,
                                     uiBlock *block,
                                     PointerRNA *ptr,
                                     PropertyRNA *prop,
                                     uiBlockCreateFunc block_func,
                                     void *block_argN,
                                     std::optional<blender::StringRef> tip,
                                     const bool use_previews,
                                     const bool editable,
                                     const bool live_icon,
                                     uiButArgNFree func_argN_free_fn = MEM_freeN,
                                     uiButArgNCopy func_argN_copy_fn = MEM_dupallocN);

uiBlock *template_common_search_menu(const bContext *C,
                                     ARegion *region,
                                     uiButSearchUpdateFn search_update_fn,
                                     void *search_arg,
                                     uiButHandleFunc search_exec_fn,
                                     void *active_item,
                                     uiButSearchTooltipFn item_tooltip_fn,
                                     const int preview_rows,
                                     const int preview_cols,
                                     float scale);

/* Zoom-related utility functions for CurveMapping. */

/**
 * Checks if zooming out is possible for the given CurveMapping.
 */
static inline bool curvemap_can_zoom_out(CurveMapping *cumap)
{
  return BLI_rctf_size_x(&cumap->curr) < BLI_rctf_size_x(&cumap->clipr);
}

/**
 * Checks if zooming in is possible for the given CurveMapping.
 */
static inline bool curvemap_can_zoom_in(CurveMapping *cumap)
{
  return BLI_rctf_size_x(&cumap->curr) > CURVE_ZOOM_MAX * BLI_rctf_size_x(&cumap->clipr);
}

/**
 * Performs a zoom-in operation on the given CurveMapping.
 */
static inline void curvemap_zoom_in(bContext *C, CurveMapping *cumap)
{
  if (curvemap_can_zoom_in(cumap)) {
    const float dx = CURVE_ZOOM_IN_SCALE_FACTOR * BLI_rctf_size_x(&cumap->curr);
    cumap->curr.xmin += dx;
    cumap->curr.xmax -= dx;
    const float dy = CURVE_ZOOM_IN_SCALE_FACTOR * BLI_rctf_size_y(&cumap->curr);
    cumap->curr.ymin += dy;
    cumap->curr.ymax -= dy;
  }

  ED_region_tag_redraw(CTX_wm_region(C));
}

/**
 * Performs a zoom-out operation on the given CurveMapping.
 */
static inline void curvemap_zoom_out(bContext *C, CurveMapping *cumap)
{
  float d, d1;

  if (curvemap_can_zoom_out(cumap)) {
    d = d1 = CURVE_ZOOM_OUT_SCALE_FACTOR * BLI_rctf_size_x(&cumap->curr);

    if (cumap->flag & CUMA_DO_CLIP) {
      if (cumap->curr.xmin - d < cumap->clipr.xmin) {
        d1 = cumap->curr.xmin - cumap->clipr.xmin;
      }
    }
    cumap->curr.xmin -= d1;

    d1 = d;
    if (cumap->flag & CUMA_DO_CLIP) {
      if (cumap->curr.xmax + d > cumap->clipr.xmax) {
        d1 = -cumap->curr.xmax + cumap->clipr.xmax;
      }
    }
    cumap->curr.xmax += d1;

    d = d1 = CURVE_ZOOM_OUT_SCALE_FACTOR * BLI_rctf_size_y(&cumap->curr);

    if (cumap->flag & CUMA_DO_CLIP) {
      if (cumap->curr.ymin - d < cumap->clipr.ymin) {
        d1 = cumap->curr.ymin - cumap->clipr.ymin;
      }
    }
    cumap->curr.ymin -= d1;

    d1 = d;
    if (cumap->flag & CUMA_DO_CLIP) {
      if (cumap->curr.ymax + d > cumap->clipr.ymax) {
        d1 = -cumap->curr.ymax + cumap->clipr.ymax;
      }
    }
    cumap->curr.ymax += d1;
  }

  ED_region_tag_redraw(CTX_wm_region(C));
}

/**
 * Helper function to configure a numeric button with unit type, step size, and precision.
 *
 * @param bt: Pointer to the button to configure.
 * @param unit_type: The unit type to set (e.g., PROP_UNIT_FREQUENCY, PROP_UNIT_DECIBEL).
 * @param step_size: The step size for the numeric input.
 * @param precision: The number of decimal places to display.
 */
static void configure_number_button(uiBut *bt, int unit_type, float step_size, int precision)
{
  UI_but_unit_type_set(bt, unit_type);
  UI_but_number_step_size_set(bt, step_size);
  UI_but_number_precision_set(bt, precision);
}

/**
 * Creates the tools UI block for curve mapping.
 *
 * @param C: The current Blender context.
 * @param region: The UI region where the block will be displayed.
 * @param cb: RNA update callback for handling changes.
 * @param show_extend: Whether to show the "Extend" buttons.
 * @param reset_mode: The reset mode for the curve (e.g., positive or negative slope).
 */
static uiBlock *curvemap_tools_func(
    bContext *C, ARegion *region, RNAUpdateCb &cb, bool show_extend, int reset_mode)
{
  PointerRNA cumap_ptr = RNA_property_pointer_get(&cb.ptr, cb.prop);
  CurveMapping *cumap = static_cast<CurveMapping *>(cumap_ptr.data);

  short yco = 0;
  const short menuwidth = 10 * UI_UNIT_X;

  uiBlock *block = UI_block_begin(C, region, __func__, UI_EMBOSS);

  {
    uiBut *but = uiDefIconTextBut(block,
                                  UI_BTYPE_BUT_MENU,
                                  1,
                                  ICON_BLANK1,
                                  IFACE_("Reset View"),
                                  0,
                                  yco -= UI_UNIT_Y,
                                  menuwidth,
                                  UI_UNIT_Y,
                                  nullptr,
                                  0.0,
                                  0.0,
                                  "");
    UI_but_func_set(but, [cumap](bContext &C) {
      BKE_curvemapping_reset_view(cumap);
      ED_region_tag_redraw(CTX_wm_region(&C));
    });
  }

  if (show_extend && !(cumap->flag & CUMA_USE_WRAPPING)) {
    {
      uiBut *but = uiDefIconTextBut(block,
                                    UI_BTYPE_BUT_MENU,
                                    1,
                                    ICON_BLANK1,
                                    IFACE_("Extend Horizontal"),
                                    0,
                                    yco -= UI_UNIT_Y,
                                    menuwidth,
                                    UI_UNIT_Y,
                                    nullptr,
                                    0.0,
                                    0.0,
                                    "");
      UI_but_func_set(but, [cumap, cb](bContext &C) {
        cumap->flag &= ~CUMA_EXTEND_EXTRAPOLATE;
        BKE_curvemapping_changed(cumap, false);
        rna_update_cb(C, cb);
        ED_undo_push(&C, "CurveMap tools");
        ED_region_tag_redraw(CTX_wm_region(&C));
      });
    }
    {
      uiBut *but = uiDefIconTextBut(block,
                                    UI_BTYPE_BUT_MENU,
                                    1,
                                    ICON_BLANK1,
                                    IFACE_("Extend Extrapolated"),
                                    0,
                                    yco -= UI_UNIT_Y,
                                    menuwidth,
                                    UI_UNIT_Y,
                                    nullptr,
                                    0.0,
                                    0.0,
                                    "");
      UI_but_func_set(but, [cumap, cb](bContext &C) {
        cumap->flag |= CUMA_EXTEND_EXTRAPOLATE;
        BKE_curvemapping_changed(cumap, false);
        rna_update_cb(C, cb);
        ED_undo_push(&C, "CurveMap tools");
        ED_region_tag_redraw(CTX_wm_region(&C));
      });
    }
  }

  {
    uiBut *but = uiDefIconTextBut(block,
                                  UI_BTYPE_BUT_MENU,
                                  1,
                                  ICON_BLANK1,
                                  IFACE_("Reset Curve"),
                                  0,
                                  yco -= UI_UNIT_Y,
                                  menuwidth,
                                  UI_UNIT_Y,
                                  nullptr,
                                  0.0,
                                  0.0,
                                  "");
    UI_but_func_set(but, [cumap, cb, reset_mode](bContext &C) {
      CurveMap *cuma = cumap->cm + cumap->cur;
      BKE_curvemap_reset(cuma, &cumap->clipr, cumap->preset, reset_mode);
      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
      ED_undo_push(&C, "CurveMap tools");
      ED_region_tag_redraw(CTX_wm_region(&C));
    });
  }

  UI_block_direction_set(block, UI_DIR_DOWN);
  UI_block_bounds_set_text(block, 3.0f * UI_UNIT_X);

  return block;
}

static uiBlock *curvemap_tools_posslope_func(bContext *C, ARegion *region, void *cb_v)
{
  return curvemap_tools_func(
      C, region, *static_cast<RNAUpdateCb *>(cb_v), true, CURVEMAP_SLOPE_POSITIVE);
}

static uiBlock *curvemap_tools_negslope_func(bContext *C, ARegion *region, void *cb_v)
{
  return curvemap_tools_func(
      C, region, *static_cast<RNAUpdateCb *>(cb_v), true, CURVEMAP_SLOPE_NEGATIVE);
}
