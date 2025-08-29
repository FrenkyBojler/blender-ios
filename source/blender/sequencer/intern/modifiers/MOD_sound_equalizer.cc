/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "BLT_translation.hh"

#include "DNA_sequence_types.h"

#include "SEQ_modifier.hh"
#include "SEQ_sound.hh"

#include "modifier.hh"

namespace blender::seq {

void sound_equalizermodifier_register(ARegionType *region_type) {}

StripModifierTypeInfo seqModifierType_SoundEqualizer = {
    /*idname*/ "SoundEqualizer",
    /*name*/ CTX_N_(BLT_I18NCONTEXT_ID_SEQUENCE, "Equalizer"),
    /*struct_name*/ "SoundEqualizerModifierData",
    /*struct_size*/ sizeof(SoundEqualizerModifierData),
    /*init_data*/ sound_equalizermodifier_init_data,
    /*free_data*/ sound_equalizermodifier_free,
    /*copy_data*/ sound_equalizermodifier_copy_data,
    /*apply*/ nullptr,
    /*panel_register*/ sound_equalizermodifier_register,
};

};  // namespace blender::seq
