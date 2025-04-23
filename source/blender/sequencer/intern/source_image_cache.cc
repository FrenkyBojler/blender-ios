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

#include "prefetch.hh"
#include "source_image_cache.hh"

namespace blender::seq {

static std::mutex source_image_cache_mutex;

struct SourceImageCache {
  struct FrameEntry {
    int frame_index = 0; /* Frame index (for movies) or image index (for image sequences). */
    ImBuf *image = nullptr;
    int64_t used_at = 0;
  };

  struct StripEntry {
    Vector<FrameEntry> frames;
  };

  Map<const Strip *, StripEntry> map_;
  int64_t logical_time_ = 0;

  ~SourceImageCache()
  {
    clear();
  }

  void clear()
  {
    for (const auto &item : map_.items()) {
      for (const auto &frame : item.value.frames) {
        IMB_freeImBuf(frame.image);
      }
    }
    map_.clear();
    logical_time_ = 0;
  }

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
};

static SourceImageCache *ensure_source_image_cache(Scene *scene)
{
  SourceImageCache **cache = &scene->ed->runtime.source_image_cache;
  if (*cache == nullptr) {
    *cache = MEM_new<SourceImageCache>(__func__);
  }
  return *cache;
}

static SourceImageCache *query_source_image_cache(Scene *scene)
{
  if (scene == nullptr || scene->ed == nullptr) {
    return nullptr;
  }
  return scene->ed->runtime.source_image_cache;
}

static Scene *scene_from_context(const RenderData *context, const Strip *&strip)
{
  Scene *scene = context->scene;
  if (context->is_prefetch_render) {
    context = seq_prefetch_get_original_context(context);
    scene = context->scene;
    strip = seq_prefetch_get_original_sequence(strip, scene);
  }
  return scene;
}

ImBuf *source_image_cache_get(const RenderData *context, const Strip *strip, float timeline_frame)
{
  if (context->skip_cache || context->is_proxy_render || strip == nullptr) {
    return nullptr;
  }

  Scene *scene = scene_from_context(context, strip);
  timeline_frame = math::round(timeline_frame);
  int frame_index = give_frame_index(scene, strip, timeline_frame);
  if (strip->type == STRIP_TYPE_MOVIE) {
    frame_index += strip->anim_startofs;
  }

  ImBuf *res = nullptr;
  {
    std::scoped_lock lock(source_image_cache_mutex);
    SourceImageCache *cache = query_source_image_cache(scene);
    if (cache == nullptr) {
      return nullptr;
    }

    int64_t cur_time = cache->logical_time_;
    SourceImageCache::StripEntry *val = cache->map_.lookup_ptr(strip);
    if (val == nullptr) {
      /* Nothing in cache for this strip yet. */
      return nullptr;
    }
    /* Search entries for the frame we want. */
    //@TODO: should this be a map instead of vector?
    for (SourceImageCache::FrameEntry &frame : val->frames) {
      if (frame.frame_index == frame_index) {
        frame.used_at = math::max(frame.used_at, cur_time);
        res = frame.image;
        break;
      }
    }
  }

  if (res) {
    IMB_refImBuf(res);
  }
  return res;
}

void source_image_cache_put(const RenderData *context,
                            const Strip *strip,
                            float timeline_frame,
                            ImBuf *image)
{
  if (context->skip_cache || context->is_proxy_render || strip == nullptr || image == nullptr) {
    return;
  }

  Scene *scene = scene_from_context(context, strip);
  timeline_frame = math::round(timeline_frame);

  int frame_index = give_frame_index(scene, strip, timeline_frame);
  if (strip->type == STRIP_TYPE_MOVIE) {
    frame_index += strip->anim_startofs;
  }

  IMB_refImBuf(image);

  std::scoped_lock lock(source_image_cache_mutex);
  SourceImageCache *cache = ensure_source_image_cache(scene);

  const int64_t cur_time = cache->logical_time_;
  SourceImageCache::StripEntry *val = cache->map_.lookup_ptr(strip);

  if (val == nullptr) {
    /* Nothing in cache for this strip yet. */
    cache->map_.add_new(strip, {});
    val = cache->map_.lookup_ptr(strip);
  }
  BLI_assert_msg(val != nullptr, "Source image cache value should never be null here");

  bool had_existing = false;
  for (SourceImageCache::FrameEntry &frame : val->frames) {
    if (frame.frame_index == frame_index) {
      frame.used_at = math::max(frame.used_at, cur_time);
      IMB_freeImBuf(frame.image);
      frame.image = image;
      had_existing = true;
      break;
    }
  }
  if (!had_existing) {
    val->frames.append({frame_index, image, cur_time});
  }
}

void source_image_cache_invalidate_strip(Scene *scene, const Strip *strip)
{
  std::scoped_lock lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache != nullptr) {
    cache->remove_entry(strip);
  }
}

void source_image_cache_clear(Scene *scene)
{
  std::scoped_lock lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache != nullptr) {
    scene->ed->runtime.source_image_cache->clear();
  }
}

void source_image_cache_destroy(Scene *scene)
{
  std::scoped_lock lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache != nullptr) {
    BLI_assert(cache == scene->ed->runtime.source_image_cache);
    MEM_delete(scene->ed->runtime.source_image_cache);
    scene->ed->runtime.source_image_cache = nullptr;
  }
}

void source_image_cache_iterate(Scene *scene,
                                void *userdata,
                                void callback_iter(void *userdata,
                                                   const Strip *strip,
                                                   int timeline_frame))
{
  std::scoped_lock lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache == nullptr) {
    return;
  }

  for (const auto &[key, value] : cache->map_.items()) {
    for (const SourceImageCache::FrameEntry &frame : value.frames) {
      /* We have frame index of source media, try to guesstimate the timeline frame.
       * Note that this will be not correct when retiming, different playback rate, strobing
       * etc. are used. */
      int timeline_frame = frame.frame_index + time_start_frame_get(key);
      callback_iter(userdata, key, timeline_frame);
    }
  }
}

size_t source_image_cache_calc_memory_size(Scene *scene)
{
  std::scoped_lock lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache == nullptr) {
    return 0;
  }
  size_t size = 0;
  for (const SourceImageCache::StripEntry &entry : cache->map_.values()) {
    for (const SourceImageCache::FrameEntry &frame : entry.frames) {
      size += IMB_get_size_in_memory(frame.image);
    }
  }
  return size;
}

bool source_image_cache_evict(Scene *scene)
{
  std::scoped_lock lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache == nullptr) {
    return false;
  }

  /* Find which entry was the least recently used. */
  SourceImageCache::StripEntry *oldest_strip = nullptr;
  int64_t oldest_index = -1;
  int64_t oldest_time = cache->logical_time_;
  for (const auto &item : cache->map_.items()) {
    for (int64_t i = 0; i < item.value.frames.size(); i++) {
      if (item.value.frames[i].used_at < oldest_time) {
        oldest_strip = &item.value;
        oldest_index = i;
        oldest_time = item.value.frames[i].used_at;
      }
    }
  }

  /* Remove if we found one. */
  if (oldest_strip != nullptr) {
    BLI_assert(oldest_index >= 0 && oldest_index < oldest_strip->frames.size());
    IMB_freeImBuf(oldest_strip->frames[oldest_index].image);
    oldest_strip->frames.remove_and_reorder(oldest_index);
    return true;
  }

  return false;
}

void source_image_cache_tick(Scene *scene)
{
  std::scoped_lock lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache != nullptr) {
    cache->logical_time_++;
  }
}

}  // namespace blender::seq
