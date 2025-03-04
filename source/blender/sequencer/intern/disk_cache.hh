/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include "BLI_listbase.h"
#include "BLI_threads.h"

struct ImBuf;
struct Main;
struct Scene;
struct SeqCacheKey;
struct Strip;

namespace blender::seq {

struct SeqDiskCache {
  Main *bmain;
  int64_t timestamp;
  ListBase files;
  ThreadMutex read_write_mutex;
  size_t size_total;
};

SeqDiskCache *seq_disk_cache_create(Main *bmain, Scene *scene);
void seq_disk_cache_free(SeqDiskCache *disk_cache);
bool seq_disk_cache_is_enabled(Main *bmain);
ImBuf *seq_disk_cache_read_file(SeqDiskCache *disk_cache, SeqCacheKey *key);
bool seq_disk_cache_write_file(SeqDiskCache *disk_cache, SeqCacheKey *key, ImBuf *ibuf);
bool seq_disk_cache_enforce_limits(SeqDiskCache *disk_cache);
void seq_disk_cache_invalidate(SeqDiskCache *disk_cache,
                               Scene *scene,
                               Strip *strip,
                               Strip *strip_changed,
                               int invalidate_types);

}  // namespace blender::seq
