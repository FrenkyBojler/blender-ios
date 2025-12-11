/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#pragma once

#include "DNA_listBase.h"

#include "BLI_map.hh"
#include "BLI_vector.hh"

struct MovieReader;
struct Strip;
struct StripModifierData;
struct PitchModifierDataRuntime;
struct EchoModifierDataRuntime;

namespace blender::deg {

struct Depsgraph;

class StripModifierDataBackup {
 public:
  StripModifierDataBackup();

  void reset();

  void init_from_modifier(StripModifierData *smd);
  void restore_to_modifier(StripModifierData *smd);

  bool isEmpty() const;

  /* For all Sound Modifiers. */
  void *sound_in;
  void *sound_out;
  int flag;
  /* For Equalizer Modifier. */
  float *last_buf;
  /* For Pitch Modifier. */
  PitchModifierDataRuntime *last_pitch_modifier;
  /* For Echo Modifier. */
  EchoModifierDataRuntime *last_echo_modifier;
};

/* Backup of a single strip. */
class StripBackup {
 public:
  StripBackup(const Depsgraph *depsgraph);

  void reset();

  void init_from_strip(Strip *strip);
  void restore_to_strip(Strip *strip);

  bool isEmpty() const;

  void *scene_sound;
  Vector<MovieReader *, 1> movie_readers;
  Map<int, StripModifierDataBackup> modifiers;
};

}  // namespace blender::deg
