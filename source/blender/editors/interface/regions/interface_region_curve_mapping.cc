/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "DNA_userdef_types.h"

#include "BKE_context.hh"

#include "UI_interface_c.hh"
#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"

#include "interface_intern.hh"

uiBlock *ui_block_func_CURVE_MAPPING(bContext *C, uiPopupBlockHandle *handle, void *arg_but)
{
  uiButCurveMapping *but_cumap = static_cast<uiButCurveMapping *>(arg_but);

  uiBlock *block = UI_block_begin(C, handle->region, __func__, blender::ui::EmbossType::Emboss);
  block->direction = UI_DIR_UP;
  block->flag = UI_BLOCK_LOOP | UI_BLOCK_KEEP_OPEN | UI_BLOCK_OUT_1;
  UI_block_theme_style_set(block, UI_BLOCK_THEME_STYLE_POPUP);
  UI_block_bounds_set_normal(block, 0.5 * UI_UNIT_X);

  const uiStyle *style = UI_style_get_dpi();
  uiLayout &layout = blender::ui::block_layout(block,
                                               blender::ui::LayoutDirection::Vertical,
                                               blender::ui::LayoutType::Panel,
                                               0,
                                               0,
                                               10 * UI_UNIT_X,
                                               0,
                                               0,
                                               style);

  uiTemplateCurveMapping(&layout,
                         &but_cumap->rnapoin,
                         RNA_property_identifier(but_cumap->rnaprop),
                         but_cumap->type,
                         but_cumap->levels,
                         but_cumap->brush,
                         but_cumap->neg_slope,
                         but_cumap->tone);

  blender::ui::block_layout_resolve(block);

  return block;
}
