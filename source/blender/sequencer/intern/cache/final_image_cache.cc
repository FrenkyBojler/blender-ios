/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "BLI_map.hh"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "IMB_imbuf.hh"

#include "SEQ_relations.hh"
#include "SEQ_render.hh"
#include "SEQ_time.hh"

#include "final_image_cache.hh"
#include "prefetch.hh"

#include <mutex>

namespace blender::seq {

static std::mutex final_image_cache_mutex;

struct FinalImageCache {
  struct FrameEntry {
    ImBuf *image = nullptr;
    int64_t used_at = 0;
  };

  Map<int, FrameEntry> map_;
  int64_t logical_time_ = 0;

  ~FinalImageCache()
  {
    clear();
  }

  void clear()
  {
    for (const auto &item : map_.values()) {
      IMB_freeImBuf(item.image);
    }
    map_.clear();
    logical_time_ = 0;
  }
};

static FinalImageCache *ensure_final_image_cache(Scene *scene)
{
  FinalImageCache **cache = &scene->ed->runtime.final_image_cache;
  if (*cache == nullptr) {
    *cache = MEM_new<FinalImageCache>(__func__);
  }
  return *cache;
}

static FinalImageCache *query_final_image_cache(const Scene *scene)
{
  if (scene == nullptr || scene->ed == nullptr) {
    return nullptr;
  }
  return scene->ed->runtime.final_image_cache;
}

ImBuf *final_image_cache_get(Scene *scene, float timeline_frame)
{
  const int key = int(math::round(timeline_frame));

  ImBuf *res = nullptr;
  {
    std::scoped_lock lock(final_image_cache_mutex);
    FinalImageCache *cache = query_final_image_cache(scene);
    if (cache == nullptr) {
      return nullptr;
    }

    int64_t cur_time = cache->logical_time_;
    FinalImageCache::FrameEntry *frame = cache->map_.lookup_ptr(key);
    if (frame == nullptr) {
      return nullptr;
    }
    frame->used_at = math::max(frame->used_at, cur_time);
    res = frame->image;
  }

  if (res) {
    IMB_refImBuf(res);
  }
  return res;
}

void final_image_cache_put(Scene *scene, float timeline_frame, ImBuf *image)
{
  const int key = int(math::round(timeline_frame));

  IMB_refImBuf(image);

  std::scoped_lock lock(final_image_cache_mutex);
  FinalImageCache *cache = ensure_final_image_cache(scene);

  const int64_t cur_time = cache->logical_time_;
  FinalImageCache::FrameEntry *existing = cache->map_.lookup_ptr(key);
  if (existing != nullptr) {
    existing->used_at = math::max(existing->used_at, cur_time);
    IMB_freeImBuf(existing->image);
    existing->image = image;
  }
  else {
    cache->map_.add_new(key, {image, cur_time});
  }
}

void final_image_cache_invalidate_frame_range(Scene *scene,
                                              const float timeline_frame_start,
                                              const float timeline_frame_end)
{
  std::scoped_lock lock(final_image_cache_mutex);
  FinalImageCache *cache = query_final_image_cache(scene);
  if (cache == nullptr) {
    return;
  }

  const int key_start = int(math::floor(timeline_frame_start));
  const int key_end = int(math::ceil(timeline_frame_end));

  for (auto it = cache->map_.items().begin(); it != cache->map_.items().end(); it++) {
    const int key = (*it).key;
    if (key >= key_start && key <= key_end) {
      IMB_freeImBuf((*it).value.image);
      cache->map_.remove(it);
    }
  }
}

void final_image_cache_clear(Scene *scene)
{
  std::scoped_lock lock(final_image_cache_mutex);
  FinalImageCache *cache = query_final_image_cache(scene);
  if (cache != nullptr) {
    scene->ed->runtime.final_image_cache->clear();
  }
}

void final_image_cache_destroy(Scene *scene)
{
  std::scoped_lock lock(final_image_cache_mutex);
  FinalImageCache *cache = query_final_image_cache(scene);
  if (cache != nullptr) {
    BLI_assert(cache == scene->ed->runtime.final_image_cache);
    MEM_delete(scene->ed->runtime.final_image_cache);
    scene->ed->runtime.final_image_cache = nullptr;
  }
}

void final_image_cache_iterate(Scene *scene,
                               void *userdata,
                               void callback_iter(void *userdata, int timeline_frame))
{
  std::scoped_lock lock(final_image_cache_mutex);
  FinalImageCache *cache = query_final_image_cache(scene);
  if (cache == nullptr) {
    return;
  }
  for (int frame : cache->map_.keys()) {
    callback_iter(userdata, frame);
  }
}

size_t final_image_cache_calc_memory_size(const Scene *scene)
{
  std::scoped_lock lock(final_image_cache_mutex);
  FinalImageCache *cache = query_final_image_cache(scene);
  if (cache == nullptr) {
    return 0;
  }
  size_t size = 0;
  for (const FinalImageCache::FrameEntry &frame : cache->map_.values()) {
    size += IMB_get_size_in_memory(frame.image);
  }
  return size;
}

bool final_image_cache_evict(Scene *scene)
{
  std::scoped_lock lock(final_image_cache_mutex);
  FinalImageCache *cache = query_final_image_cache(scene);
  if (cache == nullptr) {
    return false;
  }

  /* Find which entry was the least recently used.
   *
   * However, do not try to evict entries from the current prefetch job range -- we need to
   * be able to fully fill the cache from prefetching, and then actually stop the job when it
   * is full and no longer can evict anything. */
  int cur_prefetch_start = -1, cur_prefetch_end = -1;
  seq_prefetch_get_time_range(scene, &cur_prefetch_start, &cur_prefetch_end);

  int oldest_key = -1;
  FinalImageCache::FrameEntry *oldest_item = nullptr;
  int64_t oldest_time = cache->logical_time_;
  for (const auto &item : cache->map_.items()) {
    if (item.key >= cur_prefetch_start && item.key <= cur_prefetch_end) {
      continue; /* Within active prefetch range, do not try to remove it. */
    }
    if (item.value.used_at < oldest_time) {
      oldest_key = item.key;
      oldest_item = &item.value;
      oldest_time = item.value.used_at;
    }
  }

  /* Remove if we found one. */
  if (oldest_item != nullptr) {
    IMB_freeImBuf(oldest_item->image);
    cache->map_.remove(oldest_key);
    return true;
  }

  /* Did not find anything to remove. */
  return false;
}

void final_image_cache_tick(Scene *scene)
{
  std::scoped_lock lock(final_image_cache_mutex);
  FinalImageCache *cache = query_final_image_cache(scene);
  if (cache != nullptr) {
    cache->logical_time_++;
  }
}

}  // namespace blender::seq
