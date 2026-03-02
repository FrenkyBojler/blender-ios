/* SPDX-FileCopyrightText: 2018 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shader_fx
 */

#include "DNA_screen_types.h"
#include "DNA_shader_fx_types.h"

#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_screen.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"

#include "FX_shader_types.hh"
#include "FX_ui_common.hh"

namespace blender {


static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = shaderfx_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);

  layout.prop(ptr, "rim_color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "mask_color", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "mode", UI_ITEM_NONE, IFACE_("Blend Mode"), ICON_NONE);
  layout.prop(ptr, "offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  shaderfx_panel_end(layout, ptr);
}

static void blur_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = shaderfx_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);

  layout.prop(ptr, "blur", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "samples", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = shaderfx_panel_register(region_type, eShaderFxType_Rim, panel_draw);
  shaderfx_subpanel_register(region_type, "blur", "Blur", nullptr, blur_panel_draw, panel_type);
}

static void foreach_working_space_color(ShaderFxData *fx,
                                        const IDTypeForeachColorFunctionCallback &fn)
{
  RimShaderFxData *gpfx = reinterpret_cast<RimShaderFxData *>(fx);
  fn.single(gpfx->rim_rgb);
  fn.single(gpfx->mask_rgb);
}

ShaderFxTypeInfo shaderfx_Type_Rim = {
    /*name*/ N_("Rim"),
    /*struct_name*/ "RimShaderFxData",
    /*struct_size*/ sizeof(RimShaderFxData),
    /*type*/ eShaderFxType_GpencilType,
    /*flags*/ ShaderFxTypeFlag(0),

    /*copy_data*/ shaderfx_copy_data<RimShaderFxData>,

    /*new_data*/ shaderfx_new_data<RimShaderFxData>,
    /*free_data*/ shaderfx_free_data<RimShaderFxData>,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_working_space_color*/ foreach_working_space_color,
    /*panel_register*/ panel_register,
};

}  // namespace blender
