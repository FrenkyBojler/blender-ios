/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#include "intern/eval/deg_eval_runtime_backup_sequence.h"

#include "DNA_sequence_types.h"

#include "BLI_listbase.h"

namespace blender::deg {

StripBackup::StripBackup(const Depsgraph * /*depsgraph*/)
{
  reset();
}

void StripBackup::reset()
{
  scene_sound = nullptr;
  BLI_listbase_clear(&anims);
  sound_handle = nullptr;
  buf = nullptr;
}

void StripBackup::init_from_strip(Strip *strip)
{
  scene_sound = strip->scene_sound;
  anims = strip->anims;

  strip->scene_sound = nullptr;
  BLI_listbase_clear(&strip->anims);

  LISTBASE_FOREACH (StripModifierData *, smd, &strip->modifiers) {
    if (smd->type == seqModifierType_SoundEqualizer) {
      SoundEqualizerModifierData *semd = (SoundEqualizerModifierData *)smd;
      buf = semd->buf_backup;
      sound_handle = semd->sound_backup;
    }
  }
}

void StripBackup::restore_to_strip(Strip *strip)
{
  strip->scene_sound = scene_sound;
  strip->anims = anims;
  LISTBASE_FOREACH (StripModifierData *, smd, &strip->modifiers) {
    if (smd->type == seqModifierType_SoundEqualizer) {
      SoundEqualizerModifierData *semd = (SoundEqualizerModifierData *)smd;
      semd->buf_backup = buf;
      semd->sound_backup = sound_handle;
    }
  }

  reset();
}

bool StripBackup::isEmpty() const
{
  return (scene_sound == nullptr) && BLI_listbase_is_empty(&anims) && buf != nullptr;
}

}  // namespace blender::deg
