/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include <fmt/format.h>
#include "BKE_colortools.hh"
#include "BLI_listbase.h"
#include "BLO_read_write.hh"
#include "BLT_translation.hh"

#include "DNA_sequence_types.h"

#include "SEQ_modifier.hh"
#include "SEQ_sound.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "modifier.hh"

#include "AUD_Types.h"

namespace blender::seq {

static void pitch_shiftermodifier_init_data(StripModifierData *smd)
{
    PitchShifterModifierData *psmd = (PitchShifterModifierData *)smd;
    // 0 semi tones means no changes.
    psmd->semi_tones = 0;
    psmd->cents = 0;
    psmd->ratio = 1;
    psmd->pitch_quality = AUD_STRETCHER_QUALITY_HIGH;
}

static void pitch_shiftermodifier_draw(const bContext * /*C*/, Panel *panel)
{
    uiLayout *layout = panel->layout;
    PointerRNA *ptr = UI_panel_custom_data_get(panel);

    layout->use_property_split_set(true);

    uiLayout &col = layout->column(false);
    col.prop(ptr, "semi_tones", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "cents", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "ratio", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "pitch_quality", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void pitch_shiftermodifier_register(ARegionType *region_type)
{
    modifier_panel_register(
        region_type, eSeqModifierType_PitchShifter, pitch_shiftermodifier_draw);
}


StripModifierTypeInfo seqModifierType_PitchShifter = {
    /*idname*/ "PitchShifter",
    /*name*/ CTX_N_(BLT_I18NCONTEXT_ID_SEQUENCE, "Pitch Shifter"),
    /*struct_name*/ "PitchShifterModifierData",
    /*struct_size*/ sizeof(PitchShifterModifierData),
    /*init_data*/ pitch_shiftermodifier_init_data,
    /*free_data*/ nullptr,
    /*copy_data*/ nullptr,
    /*apply*/ nullptr,
    /*panel_register*/ pitch_shiftermodifier_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
};

};  // namespace blender::seq