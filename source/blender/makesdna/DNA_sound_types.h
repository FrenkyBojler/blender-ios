/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_defs.h"

struct Ipo;
struct PackedFile;

typedef struct bSound {
  ID id;

  /**
   * The path to the sound file.
   */
  /** 1024 = FILE_MAX. */
  char filepath[1024] = "";

  /**
   * The packed file.
   */
  struct PackedFile *packedfile = nullptr;

  /**
   * The handle for audaspace.
   */
  void *handle = nullptr;

  /**
   * Deprecated; used for loading pre 2.5 files.
   */
  struct PackedFile *newpackedfile = nullptr;
  struct Ipo *ipo = nullptr;

  float volume = 0;
  float attenuation = 0;
  float pitch = 0;
  float min_gain = 0;
  float max_gain = 0;
  float distance = 0;
  short flags = 0;
  /** Runtime only, always reset in readfile. */
  short tags = 0;
  char _pad[4] = {};
  double offset_time = 0;

  /* Unused currently. */
  // int type = 0;
  // struct bSound *child_sound = nullptr;

  /**
   * The audaspace handle for cache.
   */
  void *cache = nullptr;

  /**
   * Waveform display data.
   */
  void *waveform = nullptr;

  /**
   * The audaspace handle that should actually be played back.
   * Should be cache if cache != nullptr; otherwise its handle
   */
  void *playback_handle = nullptr;

  /** Spin-lock for asynchronous loading of sounds. */
  void *spinlock = nullptr;
  /* XXX unused currently (SOUND_TYPE_LIMITER) */
  // float start = 0, end = 0;

  /* Description of Audio channels, as of #eSoundChannels. */
  int audio_channels = 0;

  int samplerate = 0;

} bSound;

/* XXX unused currently */
#if 0
typedef enum eSound_Type {
  SOUND_TYPE_INVALID = -1,
  SOUND_TYPE_FILE = 0,
  SOUND_TYPE_BUFFER = 1,
  SOUND_TYPE_LIMITER = 2,
} eSound_Type;
#endif

/** #bSound.flags */
enum {
#ifdef DNA_DEPRECATED_ALLOW
  /* deprecated! used for sound actuator loading */
  SOUND_FLAGS_3D = (1 << 3),
#endif
  SOUND_FLAGS_CACHING = (1 << 4),
  SOUND_FLAGS_MONO = (1 << 5),
};

/** #bSound.tags */
enum {
  /* Do not free/reset waveform on sound load, only used by undo code. */
  SOUND_TAGS_WAVEFORM_NO_RELOAD = 1 << 0,
  SOUND_TAGS_WAVEFORM_LOADING = (1 << 6),
};
