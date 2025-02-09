/* SPDX-FileCopyrightText: 2016 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"

struct GSet;

/* CacheFile::type */
typedef enum {
  CACHEFILE_TYPE_ALEMBIC = 1,
  CACHEFILE_TYPE_USD = 2,
  CACHE_FILE_TYPE_INVALID = 0,
} eCacheFileType;

/* CacheFile::flag */
enum {
  CACHEFILE_DS_EXPAND = (1 << 0),
  CACHEFILE_UNUSED_0 = (1 << 1),
};

#if 0 /* UNUSED */
/* CacheFile::draw_flag */
enum {
  CACHEFILE_KEYFRAME_DRAWN = (1 << 0),
};
#endif

/* Representation of an object's path inside the archive.
 * Note that this is not a file path. */
typedef struct CacheObjectPath {
  struct CacheObjectPath *next = nullptr, *prev = nullptr;

  char path[4096] = "";
} CacheObjectPath;

/* CacheFileLayer::flag */
enum { CACHEFILE_LAYER_HIDDEN = (1 << 0) };

typedef struct CacheFileLayer {
  struct CacheFileLayer *next = nullptr, *prev = nullptr;

  /** 1024 = FILE_MAX. */
  char filepath[1024] = "";
  int flag = 0;
  int _pad = 0;
} CacheFileLayer;

/* CacheFile::velocity_unit
 * Determines what temporal unit is used to interpret velocity vectors for motion blur effects. */
enum {
  CACHEFILE_VELOCITY_UNIT_FRAME,
  CACHEFILE_VELOCITY_UNIT_SECOND,
};

typedef struct CacheFile {
  ID id;
  struct AnimData *adt = nullptr;

  /** Paths of the objects inside of the archive referenced by this CacheFile. */
  ListBase object_paths = {nullptr, nullptr};

  ListBase layers = {nullptr, nullptr};

  /** 1024 = FILE_MAX. */
  char filepath[1024] = "";

  char is_sequence = false;
  char forward_axis = 0;
  char up_axis = 0;
  char override_frame = false;

  float scale = 1.0f;
  /** The frame/time to lookup in the cache file. */
  float frame = 0.0f;
  /** The frame offset to subtract. */
  float frame_offset = 0;

  char _pad[4] = {};

  /** Animation flag. */
  short flag = 0;

  /* eCacheFileType enum. */
  char type = 0;

  /**
   * Do not load data from the cache file and display objects in the scene as boxes, Cycles will
   * load objects directly from the CacheFile. Other render engines which can load Alembic data
   * directly can take care of rendering it themselves.
   */
  char use_render_procedural = 0;

  char _pad1[3] = {};

  /** Enable data prefetching when using the Cycles Procedural. */
  char use_prefetch = 1;

  /** Size in megabytes for the prefetch cache used by the Cycles Procedural. */
  int prefetch_cache_size = 4096;

  /** Index of the currently selected layer in the UI, starts at 1. */
  int active_layer = 0;

  char _pad2[3] = {};

  char velocity_unit = 0;
  /* Name of the velocity property in the archive. */
  char velocity_name[64] = "";

  /* Runtime */
  struct CacheArchiveHandle *handle = nullptr;
  char handle_filepath[1024] = "";
  struct GSet *handle_readers = nullptr;
} CacheFile;
