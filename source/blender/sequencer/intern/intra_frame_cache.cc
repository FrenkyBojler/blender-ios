/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "IMB_imbuf.hh"

#include "intra_frame_cache.hh"

namespace blender::seq {

void invalidate_intra_frame_cache(Scene *scene, const Strip *strip)
{
  if (scene == nullptr || scene->ed == nullptr || strip == nullptr) {
    return;
  }
  IntraFrameCache *cache = scene->ed->runtime.intra_frame_cache;
  if (cache != nullptr) {
    cache->invalidate(strip);
  }
}

void IntraFrameCache::invalidate(const Strip *strip)
{
  /* Invalidate this strip, and all strips that are above it. */
  //@TODO: should also consider inputs below? via query_strip_effect_chain etc
  for (auto it = this->cache_.items().begin(); it != this->cache_.items().end(); it++) {
    const Strip *key = (*it).key;
    if (key == strip || key->machine >= strip->machine) {
      IMB_freeImBuf((*it).value);
      this->cache_.remove(it);
    }
  }
}

ImBuf *IntraFrameCache::get(const Strip *strip) const
{
  ImBuf *image = cache_.lookup_default(strip, nullptr);
  if (image != nullptr) {
    IMB_refImBuf(image);
  }
  return image;
}

void IntraFrameCache::put(const Strip *strip, ImBuf *image)
{
  BLI_assert(strip != nullptr);
  if (image == nullptr) {
    return;
  }
  ImBuf *existing = cache_.lookup_default(strip, nullptr);
  if (existing != nullptr) {
    IMB_freeImBuf(existing);
  }
  cache_.add_overwrite(strip, image);
  IMB_refImBuf(image);
}

void IntraFrameCache::clear()
{
  for (const auto &item : cache_.items()) {
    IMB_freeImBuf(item.value);
  }
  cache_.clear();
  timeline_frame_ = -1.0f;
}

}  // namespace blender::seq
