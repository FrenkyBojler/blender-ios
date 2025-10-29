/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "BLT_translation.hh"
#include <fmt/format.h>

#include "DNA_sequence_types.h"

#include "SEQ_modifier.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "modifier.hh"

#include "AUD_Types.h"

namespace blender::seq {

static void pitch_shiftmodifier_init_data(StripModifierData *smd)
{
  PitchShiftModifierData *psmd = (PitchShiftModifierData *)smd;
  // 0 semi tones means no changes.
  psmd->semi_tones = 0;
  psmd->cents = 0;
  psmd->ratio = 1;
  psmd->quality = AUD_STRETCHER_QUALITY_HIGH;
  psmd->mode = ePitchShiftMode::PITCH_SHIFT_MODE_SEMITONES;
}

static void pitch_shiftmodifier_draw(const bContext * /*C*/, Panel *panel)
{
  uiLayout *layout = panel->layout;
  PointerRNA *ptr = UI_panel_custom_data_get(panel);

  layout->use_property_split_set(true);

  uiLayout &col = layout->column(false);

  col.prop(ptr, "mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  int mode = RNA_enum_get(ptr, "mode");
  if (mode == ePitchShiftMode::PITCH_SHIFT_MODE_SEMITONES) {
    col.prop(ptr, "semi_tones", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "cents", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else if (mode == ePitchShiftMode::PITCH_SHIFT_MODE_RATIO) {
    col.prop(ptr, "ratio", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  col.prop(ptr, "quality", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void pitch_shiftmodifier_apply(ModifierApplyContext &context,
                                      StripModifierData *smd,
                                      ImBuf *mask)
{
  printf("Pitch Shift modifier cannot be applied on images.\n");
}

static void pitch_shiftmodifier_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eSeqModifierType_PitchShift, pitch_shiftmodifier_draw);
}

StripModifierTypeInfo seqModifierType_PitchShift = {
    /*idname*/ "PitchShift",
    /*name*/ CTX_N_(BLT_I18NCONTEXT_ID_SEQUENCE, "Pitch Shift"),
    /*struct_name*/ "PitchShiftModifierData",
    /*struct_size*/ sizeof(PitchShiftModifierData),
    /*init_data*/ pitch_shiftmodifier_init_data,
    /*free_data*/ nullptr,
    /*copy_data*/ nullptr,
    /*apply*/ nullptr,
    /*panel_register*/ pitch_shiftmodifier_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
};

};  // namespace blender::seq
