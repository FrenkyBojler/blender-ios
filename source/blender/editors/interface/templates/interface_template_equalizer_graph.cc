/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BKE_colortools.hh"

#include "BKE_library.hh"

#include "BLI_string_ref.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "interface_intern.hh"
#include "interface_templates_intern.hh"

using blender::StringRefNull;

/* NOTE: this is a block-menu, needs 0 events, otherwise the menu closes */
static uiBlock *equalizer_clipping_func(bContext *C, ARegion *region, void *cumap_v)
{
  CurveMapping *cumap = static_cast<CurveMapping *>(cumap_v);
  uiBut *bt;
  const float width = 8 * UI_UNIT_X;

  uiBlock *block = UI_block_begin(C, region, __func__, UI_EMBOSS);
  UI_block_flag_enable(block, UI_BLOCK_KEEP_OPEN | UI_BLOCK_MOVEMOUSE_QUIT);
  UI_block_theme_style_set(block, UI_BLOCK_THEME_STYLE_POPUP);

  bt = uiDefButBitI(block,
                    UI_BTYPE_CHECKBOX,
                    CUMA_DO_CLIP,
                    1,
                    IFACE_("Use Clipping"),
                    0,
                    5 * UI_UNIT_Y,
                    width,
                    UI_UNIT_Y,
                    &cumap->flag,
                    0.0,
                    0.0,
                    "");
  UI_but_func_set(bt, [cumap](bContext & /*C*/) { BKE_curvemapping_changed(cumap, false); });
  UI_block_align_begin(block);
  bt = uiDefButF(block,
                 UI_BTYPE_NUM,
                 0,
                 IFACE_("Min X:"),
                 0,
                 4 * UI_UNIT_Y,
                 width,
                 UI_UNIT_Y,
                 &cumap->clipr.xmin,
                 MIN_FREQUENCY_X,
                 cumap->clipr.xmax,
                 "");
  UI_but_func_set(bt, [cumap](bContext & /*C*/) { BKE_curvemapping_changed(cumap, false); });
  configure_number_button(bt, PROP_UNIT_FREQUENCY, 10, 2);
  bt = uiDefButF(block,
                 UI_BTYPE_NUM,
                 0,
                 IFACE_("Max X:"),
                 0,
                 3 * UI_UNIT_Y,
                 width,
                 UI_UNIT_Y,
                 &cumap->clipr.xmax,
                 cumap->clipr.xmin,
                 MAX_FREQUENCY_X,
                 "");
  UI_but_func_set(bt, [cumap](bContext & /*C*/) { BKE_curvemapping_changed(cumap, false); });
  configure_number_button(bt, PROP_UNIT_FREQUENCY, 10, 2);
  bt = uiDefButF(block,
                 UI_BTYPE_NUM,
                 0,
                 IFACE_("Min Y:"),
                 0,
                 2 * UI_UNIT_Y,
                 width,
                 UI_UNIT_Y,
                 &cumap->clipr.ymin,
                 MIN_DECIBEL_Y,
                 cumap->clipr.ymax,
                 "");
  UI_but_func_set(bt, [cumap](bContext & /*C*/) { BKE_curvemapping_changed(cumap, false); });
  configure_number_button(bt, PROP_UNIT_DECIBEL, 10, 2);
  bt = uiDefButF(block,
                 UI_BTYPE_NUM,
                 0,
                 IFACE_("Max Y:"),
                 0,
                 UI_UNIT_Y,
                 width,
                 UI_UNIT_Y,
                 &cumap->clipr.ymax,
                 cumap->clipr.ymin,
                 MAX_DECIBEL_Y,
                 "");
  UI_but_func_set(bt, [cumap](bContext & /*C*/) { BKE_curvemapping_changed(cumap, false); });
  configure_number_button(bt, PROP_UNIT_DECIBEL, 10, 2);

  UI_block_bounds_set_normal(block, 0.3f * U.widget_unit);
  UI_block_direction_set(block, UI_DIR_DOWN);

  return block;
}

/**
 * Layouts and initializes the Equalizer buttons in the UI.
 *
 * @param layout: Pointer to the layout structure defining the arrangement of UI elements.
 * @param ptr: RNA pointer used to access the CurveMapping data for the equalizer curve.
 * @param neg_slope: Boolean indicating if the negative slope mode is enabled.
 * @param cb: Callback function for updating RNA data when a button is triggered.
 */
static void equalizer_buttons_layout(uiLayout *layout,
                                     PointerRNA *ptr,
                                     bool neg_slope,
                                     const RNAUpdateCb &cb)
{
  CurveMapping *cumap = static_cast<CurveMapping *>(ptr->data);
  CurveMap *cm = &cumap->cm[cumap->cur];
  uiBut *bt;
  const float dx = UI_UNIT_X;
  eButGradientType bg = UI_GRAD_NONE;

  uiBlock *block = uiLayoutGetBlock(layout);

  UI_block_emboss_set(block, UI_EMBOSS);

  /* curve chooser */
  uiLayout *row = uiLayoutRow(layout, false);

  /* operation buttons */
  /* (Right aligned) */
  uiLayout *sub = uiLayoutRow(row, true);
  uiLayoutSetAlignment(sub, UI_LAYOUT_ALIGN_RIGHT);

  if (!(cumap->flag & CUMA_USE_WRAPPING)) {
    /* Zoom in */
    uiBut *bt = uiDefIconBut(block,
                             UI_BTYPE_BUT,
                             0,
                             ICON_ZOOM_IN,
                             0,
                             0,
                             UI_UNIT_X,
                             UI_UNIT_X,
                             nullptr,
                             0.0,
                             0.0,
                             TIP_("Zoom in"));
    UI_but_func_set(bt, [cumap](bContext &C) { curvemap_zoom_in(&C, cumap); });
    if (!curvemap_can_zoom_in(cumap)) {
      UI_but_disable(bt, "");
    }

    /* Zoom out */
    bt = uiDefIconBut(block,
                      UI_BTYPE_BUT,
                      0,
                      ICON_ZOOM_OUT,
                      0,
                      0,
                      UI_UNIT_X,
                      UI_UNIT_X,
                      nullptr,
                      0.0,
                      0.0,
                      TIP_("Zoom out"));
    UI_but_func_set(bt, [cumap](bContext &C) { curvemap_zoom_out(&C, cumap); });
    if (!curvemap_can_zoom_out(cumap)) {
      UI_but_disable(bt, "");
    }

    /* Clipping button. */
    const int icon = (cumap->flag & CUMA_DO_CLIP) ? ICON_CLIPUV_HLT : ICON_CLIPUV_DEHLT;
    bt = uiDefIconBlockBut(
        block, equalizer_clipping_func, cumap, 0, icon, 0, 0, dx, dx, TIP_("Clipping Options"));
    bt->drawflag &= ~UI_BUT_ICON_LEFT;
    UI_but_func_set(bt, [cb](bContext &C) { rna_update_cb(C, cb); });
  }

  RNAUpdateCb *tools_cb = MEM_new<RNAUpdateCb>(__func__, cb);
  if (neg_slope) {
    bt = uiDefIconBlockBut(
        block, curvemap_tools_negslope_func, tools_cb, 0, ICON_NONE, 0, 0, dx, dx, TIP_("Tools"));
  }
  else {
    bt = uiDefIconBlockBut(
        block, curvemap_tools_posslope_func, tools_cb, 0, ICON_NONE, 0, 0, dx, dx, TIP_("Tools"));
  }
  /* Pass ownership of `tools_cb` to the button. */
  UI_but_funcN_set(
      bt,
      [](bContext *, void *, void *) {},
      tools_cb,
      nullptr,
      but_func_argN_free<RNAUpdateCb>,
      but_func_argN_copy<RNAUpdateCb>);

  UI_block_funcN_set(block,
                     rna_update_cb,
                     MEM_new<RNAUpdateCb>(__func__, cb),
                     nullptr,
                     but_func_argN_free<RNAUpdateCb>,
                     but_func_argN_copy<RNAUpdateCb>);

  /* Curve itself. */
  const int size = max_ii(uiLayoutGetWidth(layout), UI_UNIT_X);
  row = uiLayoutRow(layout, false);
  uiButCurveMapping *curve_but = (uiButCurveMapping *)uiDefBut(
      block, UI_BTYPE_CURVE, 0, "", 0, 0, size, 8.0f * UI_UNIT_X, cumap, 0.0f, 1.0f, "");
  curve_but->gradient_type = bg;

  /* Sliders for selected curve point. */
  int i;
  CurveMapPoint *cmp = nullptr;
  bool point_last_or_first = false;
  for (i = 0; i < cm->totpoint; i++) {
    if (cm->curve[i].flag & CUMA_SELECT) {
      cmp = &cm->curve[i];
      break;
    }
  }
  /* Check if the selected curve point is the first or last point. */
  if (ELEM(i, 0, cm->totpoint - 1)) {
    point_last_or_first = true;
  }

  if (cmp) {
    rctf bounds;
    if (cumap->flag & CUMA_DO_CLIP) {
      bounds = cumap->clipr;
    }
    else {
      bounds.xmin = bounds.ymin = DEFAULT_BOUNDS_MIN;
      bounds.xmax = bounds.ymax = DEFAULT_BOUNDS_MAX;
    }

    UI_block_emboss_set(block, UI_EMBOSS);

    uiLayoutRow(layout, true);

    /* Curve handle buttons */
    /* Curve handle buttons. */
    bt = uiDefIconBut(block,
                      UI_BTYPE_BUT,
                      1,
                      ICON_HANDLE_AUTO,
                      0,
                      UI_UNIT_Y,
                      UI_UNIT_X,
                      UI_UNIT_Y,
                      nullptr,
                      0.0,
                      0.0,
                      TIP_("Auto Handle"));
    UI_but_func_set(bt, [cumap, cb](bContext &C) {
      CurveMap *cuma = cumap->cm + cumap->cur;
      BKE_curvemap_handle_set(cuma, HD_AUTO);
      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
    });
    if (((cmp->flag & CUMA_HANDLE_AUTO_ANIM) == false) &&
        ((cmp->flag & CUMA_HANDLE_VECTOR) == false))
    {
      bt->flag |= UI_SELECT_DRAW;
    }

    bt = uiDefIconBut(block,
                      UI_BTYPE_BUT,
                      1,
                      ICON_HANDLE_VECTOR,
                      0,
                      UI_UNIT_Y,
                      UI_UNIT_X,
                      UI_UNIT_Y,
                      nullptr,
                      0.0,
                      0.0,
                      TIP_("Vector Handle"));
    UI_but_func_set(bt, [cumap, cb](bContext &C) {
      CurveMap *cuma = cumap->cm + cumap->cur;
      BKE_curvemap_handle_set(cuma, HD_VECT);
      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
    });
    if (cmp->flag & CUMA_HANDLE_VECTOR) {
      bt->flag |= UI_SELECT_DRAW;
    }

    bt = uiDefIconBut(block,
                      UI_BTYPE_BUT,
                      1,
                      ICON_HANDLE_AUTOCLAMPED,
                      0,
                      UI_UNIT_Y,
                      UI_UNIT_X,
                      UI_UNIT_Y,
                      nullptr,
                      0.0,
                      0.0,
                      TIP_("Auto Clamped"));
    UI_but_func_set(bt, [cumap, cb](bContext &C) {
      CurveMap *cuma = cumap->cm + cumap->cur;
      BKE_curvemap_handle_set(cuma, HD_AUTO_ANIM);
      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
    });
    if (cmp->flag & CUMA_HANDLE_AUTO_ANIM) {
      bt->flag |= UI_SELECT_DRAW;
    }

    /* Curve handle position */
    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   0,
                   "X:",
                   0,
                   2 * UI_UNIT_Y,
                   UI_UNIT_X * 10,
                   UI_UNIT_Y,
                   &cmp->x,
                   bounds.xmin,
                   bounds.xmax,
                   "");
    configure_number_button(bt, PROP_UNIT_FREQUENCY, 10, 2);
    UI_but_func_set(bt, [cumap, cb](bContext &C) {
      BKE_curvemapping_changed(cumap, true);
      rna_update_cb(C, cb);
    });

    bt = uiDefButF(block,
                   UI_BTYPE_NUM,
                   0,
                   "Y:",
                   0,
                   1 * UI_UNIT_Y,
                   UI_UNIT_X * 10,
                   UI_UNIT_Y,
                   &cmp->y,
                   bounds.ymin,
                   bounds.ymax,
                   "");

    configure_number_button(bt, PROP_UNIT_DECIBEL, 10, 2);
    UI_but_func_set(bt, [cumap, cb](bContext &C) {
      BKE_curvemapping_changed(cumap, true);
      rna_update_cb(C, cb);
    });

    /* Curve handle delete point */
    bt = uiDefIconBut(block,
                      UI_BTYPE_BUT,
                      0,
                      ICON_TRASH,
                      0,
                      0,
                      dx,
                      dx,
                      nullptr,
                      0.0,
                      0.0,
                      TIP_("Delete points"));
    UI_but_func_set(bt, [cumap, cb](bContext &C) {
      BKE_curvemap_remove(cumap->cm + cumap->cur, SELECT);
      BKE_curvemapping_changed(cumap, false);
      rna_update_cb(C, cb);
    });
    if (point_last_or_first) {
      UI_but_flag_enable(bt, UI_BUT_DISABLED);
    }
  }

  UI_block_funcN_set(block, nullptr, nullptr, nullptr);
}

void uiTemplateSoundEqualizerMapping(uiLayout *layout,
                                     PointerRNA *ptr,
                                     const StringRefNull propname,
                                     bool neg_slope)
{
  PropertyRNA *prop = RNA_struct_find_property(ptr, propname.c_str());
  uiBlock *block = uiLayoutGetBlock(layout);

  if (!prop) {
    RNA_warning(
        "curve property not found: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  if (RNA_property_type(prop) != PROP_POINTER) {
    RNA_warning(
        "curve is not a pointer: %s.%s", RNA_struct_identifier(ptr->type), propname.c_str());
    return;
  }

  PointerRNA cptr = RNA_property_pointer_get(ptr, prop);
  if (!cptr.data || !RNA_struct_is_a(cptr.type, &RNA_CurveMapping)) {
    return;
  }

  ID *id = cptr.owner_id;
  UI_block_lock_set(block, (id && !ID_IS_EDITABLE(id)), ERROR_LIBDATA_MESSAGE);

  equalizer_buttons_layout(layout, &cptr, neg_slope, RNAUpdateCb{*ptr, prop});

  UI_block_lock_clear(block);
}
