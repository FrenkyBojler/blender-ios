/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"

#include "UI_interface_c.hh"
#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"

#include "BLT_translation.hh"

#include "IMB_colormanagement.hh"

#include "interface_intern.hh"

uiBlock *ui_block_func_CURVE_MAPPING(bContext *C, uiPopupBlockHandle *handle, void *arg_but)
{
  uiBut *but = static_cast<uiBut *>(arg_but);
  uiButCurveMapping *but_cumap = static_cast<uiButCurveMapping *>(but);

  uiBlock *block = UI_block_begin(C, handle->region, __func__, blender::ui::EmbossType::Emboss);
  block->direction = UI_DIR_UP;

  uiLayout &layout = blender::ui::block_layout(block,
                                               blender::ui::LayoutDirection::Vertical,
                                               blender::ui::LayoutType::Panel,
                                               100,
                                               100,
                                               200,
                                               1,
                                               0,
                                               UI_style_get_dpi());

  uiTemplateCurveMapping(&layout,
                         &but->rnapoin,
                         RNA_property_identifier(but->rnaprop),
                         0,
                         false,
                         false,
                         false,
                         false);

  layout.label("Hello World", ICON_NONE);
  blender::ui::block_layout_resolve(block);

  return block;
}
