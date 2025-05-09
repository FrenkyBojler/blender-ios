/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "BLI_map.hh"
#include "BLI_mutex.hh"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "IMB_imbuf.hh"

#include "SEQ_relations.hh"
#include "SEQ_render.hh"
#include "SEQ_time.hh"

#include "prefetch.hh"
#include "source_image_cache.hh"

namespace blender::seq {

static Mutex source_image_cache_mutex;

struct SourceImageCache {
  struct FrameEntry {
    ImBuf *image = nullptr;
    int64_t used_at = 0;
  };

  struct StripEntry {
    /* Map key is {source media frame index (i.e. movie frame), view ID}. */
    Map<std::pair<int, int>, FrameEntry> frames;
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
      for (const auto &frame : item.value.frames.values()) {
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
    for (const auto &frame : entry->frames.values()) {
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

static SourceImageCache *query_source_image_cache(const Scene *scene)
{
  if (scene == nullptr || scene->ed == nullptr) {
    return nullptr;
  }
  return scene->ed->runtime.source_image_cache;
}

ImBuf *source_image_cache_get(const RenderData *context, const Strip *strip, float timeline_frame)
{
  if (context->skip_cache || context->is_proxy_render || strip == nullptr) {
    return nullptr;
  }

  Scene *scene = prefetch_get_original_scene_and_strip(context, strip);
  timeline_frame = math::round(timeline_frame);
  int frame_index = give_frame_index(scene, strip, timeline_frame);
  if (strip->type == STRIP_TYPE_MOVIE) {
    frame_index += strip->anim_startofs;
  }
  const int view_id = context->view_id;

  ImBuf *res = nullptr;
  {
    std::lock_guard lock(source_image_cache_mutex);
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
    SourceImageCache::FrameEntry *frame = val->frames.lookup_ptr({frame_index, view_id});
    if (frame != nullptr) {
      frame->used_at = math::max(frame->used_at, cur_time);
      res = frame->image;
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

  Scene *scene = prefetch_get_original_scene_and_strip(context, strip);
  timeline_frame = math::round(timeline_frame);

  int frame_index = give_frame_index(scene, strip, timeline_frame);
  if (strip->type == STRIP_TYPE_MOVIE) {
    frame_index += strip->anim_startofs;
  }
  const int view_id = context->view_id;

  IMB_refImBuf(image);

  std::lock_guard lock(source_image_cache_mutex);
  SourceImageCache *cache = ensure_source_image_cache(scene);

  const int64_t cur_time = cache->logical_time_;
  SourceImageCache::StripEntry *val = cache->map_.lookup_ptr(strip);

  if (val == nullptr) {
    /* Nothing in cache for this strip yet. */
    cache->map_.add_new(strip, {});
    val = cache->map_.lookup_ptr(strip);
  }
  BLI_assert_msg(val != nullptr, "Source image cache value should never be null here");

  SourceImageCache::FrameEntry &frame = val->frames.lookup_or_add_default({frame_index, view_id});
  if (frame.image != nullptr) {
    IMB_freeImBuf(frame.image);
  }
  frame.used_at = math::max(frame.used_at, cur_time);
  frame.image = image;
}

void source_image_cache_invalidate_strip(Scene *scene, const Strip *strip)
{
  std::lock_guard lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache != nullptr) {
    cache->remove_entry(strip);
  }
}

void source_image_cache_clear(Scene *scene)
{
  std::lock_guard lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache != nullptr) {
    scene->ed->runtime.source_image_cache->clear();
  }
}

void source_image_cache_destroy(Scene *scene)
{
  std::lock_guard lock(source_image_cache_mutex);
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
  std::lock_guard lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache == nullptr) {
    return;
  }

  const float scene_fps = float(scene->r.frs_sec) / float(scene->r.frs_sec_base);

  for (const auto &[key, value] : cache->map_.items()) {
    for (std::pair<int, int> frame_view : value.frames.keys()) {
      /* We have frame index of source media, try to guesstimate the timeline frame.
       * Note that this will be not correct when retiming, strobing etc. are used.
       * However, factor in playback rate difference. */
      float frame_fl = frame_view.first / time_media_playback_rate_factor_get(key, scene_fps);
      float timeline_frame = frame_fl + time_start_frame_get(key);

      callback_iter(userdata, key, int(timeline_frame));
    }
  }
}

size_t source_image_cache_calc_memory_size(const Scene *scene)
{
  std::lock_guard lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache == nullptr) {
    return 0;
  }
  size_t size = 0;
  for (const SourceImageCache::StripEntry &entry : cache->map_.values()) {
    for (const SourceImageCache::FrameEntry &frame : entry.frames.values()) {
      size += IMB_get_size_in_memory(frame.image);
    }
  }
  return size;
}

size_t source_image_cache_get_image_count(const Scene *scene)
{
  std::lock_guard lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache == nullptr) {
    return 0;
  }
  size_t count = 0;
  for (const SourceImageCache::StripEntry &entry : cache->map_.values()) {
    count += entry.frames.size();
  }
  return count;
}

bool source_image_cache_evict(Scene *scene)
{
  std::lock_guard lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache == nullptr) {
    return false;
  }

  /* Find which entry was the least recently used. */
  SourceImageCache::StripEntry *oldest_strip = nullptr;
  std::pair<int, int> oldest_key = {};
  int64_t oldest_time = cache->logical_time_;
  for (const auto &item : cache->map_.items()) {
    for (const auto &frame : item.value.frames.items()) {
      if (frame.value.used_at < oldest_time) {
        oldest_strip = &item.value;
        oldest_key = frame.key;
        oldest_time = frame.value.used_at;
      }
    }
  }

  /* Remove if we found one. */
  if (oldest_strip != nullptr) {
    IMB_freeImBuf(oldest_strip->frames.lookup(oldest_key).image);
    oldest_strip->frames.remove(oldest_key);
    return true;
  }

  return false;
}

void source_image_cache_tick(Scene *scene)
{
  std::lock_guard lock(source_image_cache_mutex);
  SourceImageCache *cache = query_source_image_cache(scene);
  if (cache != nullptr) {
    cache->logical_time_++;
  }
}

}  // namespace blender::seq
