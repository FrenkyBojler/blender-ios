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

#include "SEQ_render.hh"
#include "SEQ_time.hh"

#include "final_image_cache.hh"
#include "prefetch.hh"

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

  /*
  void remove_entry(const Strip *strip)
  {
    StripEntry *entry = map_.lookup_ptr(strip);
    if (entry == nullptr) {
      return;
    }
    for (const auto &frame : entry->frames) {
      IMB_freeImBuf(frame.image);
    }
    map_.remove_contained(strip);
  }
   */
};

static FinalImageCache *ensure_final_image_cache(Scene *scene)
{
  FinalImageCache **cache = &scene->ed->runtime.final_image_cache;
  if (*cache == nullptr) {
    *cache = MEM_new<FinalImageCache>(__func__);
  }
  return *cache;
}

static FinalImageCache *query_final_image_cache(Scene *scene)
{
  if (scene == nullptr || scene->ed == nullptr) {
    return nullptr;
  }
  return scene->ed->runtime.final_image_cache;
}

static Scene *scene_from_context(const RenderData *context)
{
  Scene *scene = context->scene;
  if (context->is_prefetch_render) {
    context = seq_prefetch_get_original_context(context);
    scene = context->scene;
  }
  return scene;
}

ImBuf *final_image_cache_get(const RenderData *context, float timeline_frame)
{
  Scene *scene = scene_from_context(context);
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

void final_image_cache_put(const RenderData *context, float timeline_frame, ImBuf *image)
{
  if (image == nullptr) {
    return;
  }

  Scene *scene = scene_from_context(context);
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

void final_image_cache_maintain_capacity(Scene *scene)
{
  //@TODO
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
  for (auto it = cache->map_.items().begin(); it != cache->map_.items().end(); it++) {
    const int key = (*it).key;
    callback_iter(userdata, key);
  }
}

}  // namespace blender::seq
