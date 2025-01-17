/* SPDX-FileCopyrightText: 2021-2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "SEQ_sequencer.hh"
#include "sequencer.hh"

#include "DNA_listBase.h"
#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_vector_set.hh"

#include <cstring>
#include <mutex>

#include "MEM_guardedalloc.h"

static std::mutex lookup_lock;

struct StripLookup {
  blender::Map<std::string, Strip *> strip_by_name;
  blender::Map<const Strip *, Strip *> meta_by_strip;
  blender::Map<const Strip *, blender::VectorSet<Strip *>> effects_by_strip;
  blender::Map<const SeqTimelineChannel *, Strip *> owner_by_channel;
  bool valid_strip_by_name = false;
  bool valid_meta_by_strip = false;
  bool valid_effects_by_strip = false;
  bool valid_owner_by_channel = false;

  void build_strip_by_name(const ListBase *seqbase);
  void build_meta_by_strip(Strip *parent_meta, const ListBase *seqbase);
  void build_effects_by_strip(const ListBase *seqbase);
  void build_owner_by_channel(Strip *parent_meta, const ListBase *seqbase);

  void append_effect(const Strip *input, Strip *effect);
};

void StripLookup::append_effect(const Strip *input, Strip *effect)
{
  if (input != nullptr) {
    blender::VectorSet<Strip *> &effects = this->effects_by_strip.lookup_or_add_default(input);
    effects.add(effect);
  }
}

void StripLookup::build_strip_by_name(const ListBase *seqbase)
{
  LISTBASE_FOREACH (Strip *, strip, seqbase) {
    this->strip_by_name.add(strip->name + 2, strip);
    if (strip->type == STRIP_TYPE_META) {
      build_strip_by_name(&strip->seqbase);
    }
  }
}

void StripLookup::build_meta_by_strip(Strip *parent_meta, const ListBase *seqbase)
{
  LISTBASE_FOREACH (Strip *, strip, seqbase) {
    this->meta_by_strip.add(strip, parent_meta);
    if (strip->type == STRIP_TYPE_META) {
      build_meta_by_strip(strip, &strip->seqbase);
    }
  }
}

void StripLookup::build_effects_by_strip(const ListBase *seqbase)
{
  LISTBASE_FOREACH (Strip *, strip, seqbase) {
    if ((strip->type & STRIP_TYPE_EFFECT) != 0) {
      this->append_effect(strip->seq1, strip);
      this->append_effect(strip->seq2, strip);
    }
    if (strip->type == STRIP_TYPE_META) {
      build_effects_by_strip(&strip->seqbase);
    }
  }
}

void StripLookup::build_owner_by_channel(Strip *parent_meta, const ListBase *seqbase)
{
  if (parent_meta != nullptr) {
    LISTBASE_FOREACH (SeqTimelineChannel *, channel, &parent_meta->channels) {
      this->owner_by_channel.add(channel, parent_meta);
    }
  }

  LISTBASE_FOREACH (Strip *, strip, seqbase) {
    if (strip->type == STRIP_TYPE_META) {
      build_owner_by_channel(strip, &strip->seqbase);
    }
  }
}

static void ensure_strip_lookup(StripLookup *&r_lookup)
{
  if (r_lookup == nullptr) {
    r_lookup = MEM_new<StripLookup>(__func__);
  }
}

static void strip_lookup_update_strip_by_name(const Scene *scene, StripLookup *&r_lookup)
{
  ensure_strip_lookup(r_lookup);
  if (r_lookup->valid_strip_by_name) {
    return;
  }

  /* Clear previous map, but keep a portion of previous allocation capacity. */
  int64_t new_capacity = r_lookup->strip_by_name.capacity() / 8;
  r_lookup->strip_by_name.clear();
  r_lookup->strip_by_name.reserve(new_capacity);

  r_lookup->build_strip_by_name(&scene->ed->seqbase);
  r_lookup->valid_strip_by_name = true;
}

static void strip_lookup_update_meta_by_strip(const Scene *scene, StripLookup *&r_lookup)
{
  ensure_strip_lookup(r_lookup);
  if (r_lookup->valid_meta_by_strip) {
    return;
  }

  /* Clear previous map, but keep a portion of previous allocation capacity. */
  int64_t new_capacity = r_lookup->meta_by_strip.capacity() / 8;
  r_lookup->meta_by_strip.clear();
  r_lookup->meta_by_strip.reserve(new_capacity);

  r_lookup->build_meta_by_strip(nullptr, &scene->ed->seqbase);
  r_lookup->valid_meta_by_strip = true;
}

static void strip_lookup_update_effects_by_strip(const Scene *scene, StripLookup *&r_lookup)
{
  ensure_strip_lookup(r_lookup);
  if (r_lookup->valid_effects_by_strip) {
    return;
  }

  /* Clear previous map, but keep a portion of previous allocation capacity. */
  int64_t new_capacity = r_lookup->effects_by_strip.capacity() / 8;
  r_lookup->effects_by_strip.clear();
  r_lookup->effects_by_strip.reserve(new_capacity);

  r_lookup->build_effects_by_strip(&scene->ed->seqbase);
  r_lookup->valid_effects_by_strip = true;
}

static void strip_lookup_update_owner_by_channel(const Scene *scene, StripLookup *&r_lookup)
{
  ensure_strip_lookup(r_lookup);
  if (r_lookup->valid_owner_by_channel) {
    return;
  }

  /* Clear previous map, but keep a portion of previous allocation capacity. */
  int64_t new_capacity = r_lookup->owner_by_channel.capacity() / 8;
  r_lookup->owner_by_channel.clear();
  r_lookup->owner_by_channel.reserve(new_capacity);

  r_lookup->build_owner_by_channel(nullptr, &scene->ed->seqbase);
  r_lookup->valid_owner_by_channel = true;
}

void SEQ_strip_lookup_free(const Scene *scene)
{
  BLI_assert(scene->ed);
  std::lock_guard lock(lookup_lock);
  MEM_delete(scene->ed->runtime.strip_lookup);
  scene->ed->runtime.strip_lookup = nullptr;
}

Strip *SEQ_lookup_strip_by_name(const Scene *scene, const char *key)
{
  BLI_assert(scene->ed);
  std::lock_guard lock(lookup_lock);
  StripLookup *&lookup = scene->ed->runtime.strip_lookup;
  strip_lookup_update_strip_by_name(scene, lookup);
  return lookup->strip_by_name.lookup_default(key, nullptr);
}

Strip *SEQ_lookup_meta_by_strip(const Scene *scene, const Strip *key)
{
  BLI_assert(scene->ed);
  std::lock_guard lock(lookup_lock);
  StripLookup *&lookup = scene->ed->runtime.strip_lookup;
  strip_lookup_update_meta_by_strip(scene, lookup);
  return lookup->meta_by_strip.lookup_default(key, nullptr);
}

blender::Span<Strip *> SEQ_lookup_effects_by_strip(const Scene *scene, const Strip *key)
{
  BLI_assert(scene->ed);
  std::lock_guard lock(lookup_lock);
  StripLookup *&lookup = scene->ed->runtime.strip_lookup;
  strip_lookup_update_effects_by_strip(scene, lookup);
  blender::VectorSet<Strip *> &effects = lookup->effects_by_strip.lookup_or_add_default(key);
  return effects.as_span();
}

Strip *SEQ_lookup_channel_owner(const Scene *scene, const SeqTimelineChannel *channel)
{
  BLI_assert(scene->ed);
  std::lock_guard lock(lookup_lock);
  StripLookup *&lookup = scene->ed->runtime.strip_lookup;
  strip_lookup_update_owner_by_channel(scene, lookup);
  return lookup->owner_by_channel.lookup_default(channel, nullptr);
}

void SEQ_strip_lookup_invalidate(const Scene *scene, StripLookupInvalidateFlag flags)
{
  if (scene == nullptr || scene->ed == nullptr) {
    return;
  }

  std::lock_guard lock(lookup_lock);
  StripLookup *lookup = scene->ed->runtime.strip_lookup;
  if (lookup != nullptr) {
    if ((flags & StripLookupInvalidateFlag::Name) != StripLookupInvalidateFlag::None) {
      lookup->valid_strip_by_name = false;
    }
    if ((flags & StripLookupInvalidateFlag::Meta) != StripLookupInvalidateFlag::None) {
      lookup->valid_meta_by_strip = false;
    }
    if ((flags & StripLookupInvalidateFlag::Effects) != StripLookupInvalidateFlag::None) {
      lookup->valid_effects_by_strip = false;
    }
    if ((flags & StripLookupInvalidateFlag::Channel) != StripLookupInvalidateFlag::None) {
      lookup->valid_owner_by_channel = false;
    }
  }
}
