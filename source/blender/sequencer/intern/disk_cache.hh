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

struct ImBuf;
struct Main;
struct Scene;
struct Strip;

namespace blender::seq {
struct CacheKey;
struct DiskCache;

DiskCache *disk_cache_create(Main *bmain, Scene *scene);
void disk_cache_free(DiskCache *disk_cache);
bool disk_cache_is_enabled(Main *bmain);
ImBuf *disk_cache_read_file(DiskCache *disk_cache, CacheKey *key);
bool disk_cache_write_file(DiskCache *disk_cache, CacheKey *key, ImBuf *ibuf);
bool disk_cache_enforce_limits(DiskCache *disk_cache);
void disk_cache_invalidate(
    DiskCache *disk_cache, Scene *scene, Strip *strip, Strip *strip_changed, int invalidate_types);

}  // namespace blender::seq
