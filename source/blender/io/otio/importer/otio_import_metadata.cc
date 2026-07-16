/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <any>
#include <concepts>
#include <string>

#include "BLI_string.hh"

#include "DNA_sound_types.h"

#include <opentimelineio/anyDictionary.h>
#include <opentimelineio/anyVector.h>

#include "otio_import_metadata.hh"

namespace blender::io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

/**
 * \return `true` if any_cast is successful
 */
template<typename T>
static bool any_cast_set(std::any any_value, T &destination, size_t size_max = 64)
{
  try {
    if constexpr (std::is_same_v<T, AnyDictionary> || std::is_same_v<T, AnyVector>) {
      destination = std::any_cast<T &>(any_value);
    }
    else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
      destination = static_cast<T>(std::any_cast<double>(any_value));
    }
    else if constexpr (std::is_same_v<T, bool>) {
      destination = std::any_cast<T>(any_value);
    }
    else if constexpr (std::is_same_v<T, std::string>) {
      destination = std::any_cast<std::string &>(any_value);
    }
    else if constexpr (std::is_same_v<T, char *>) {
      std::string &value = std::any_cast<std::string &>(any_value);
      BLI_strncpy(destination, value.c_str(), size_max);
    }
    else {
      /* int, short, enum. */
      destination = static_cast<T>(std::any_cast<int64_t>(any_value));
    }

    return true;
  }
  catch (const std::bad_any_cast &e) {
  }

  return false;
}

/**
 * \return `true` if any_cast is successful
 */
template<typename T>
static bool any_cast_set(AnyDictionary &any_dict,
                         const char *key,
                         T &destination,
                         size_t size_max = 64)
{
  if (!any_dict.has_key(key)) {
    return false;
  }
  return any_cast_set(any_dict[key], destination, size_max);
}

template<typename T>
concept BitwiseOperable = std::integral<T> ||
                          (std::is_enum_v<T> && std::integral<std::underlying_type_t<T>>);

/**
 * \return `true` if any_cast is successful
 */
template<BitwiseOperable T>
bool any_cast_set_flag(AnyDictionary &any_dict, const char *key, T &destination, T flag)
{
  bool value = false;

  if (!any_cast_set(any_dict, key, value)) {
    return false;
  }

  if (value) {
    destination |= flag;
  }
  else {
    destination &= ~flag;
  }
  return true;
}

TransitionMetadata fetch_transition_metadata(Transition *transition)
{
  TransitionMetadata transition_metadata;
  AnyDictionary metadata;

  if (!any_cast_set(transition->metadata(), "blender", metadata)) {
    return transition_metadata;
  }

  std::string name;
  any_cast_set(metadata, "name", name);
  if (name == "Wipe") {
    transition_metadata.type = STRIP_TYPE_WIPE;
  }
  else if (name == "Gamma Crossfade") {
    transition_metadata.type = STRIP_TYPE_GAMCROSS;
  }

  any_cast_set(metadata, "default_fade", transition_metadata.default_fade);
  any_cast_set(metadata, "effect_fader", transition_metadata.effect_fader);
  any_cast_set(metadata, "edgeWidth", transition_metadata.edgeWidth);
  any_cast_set(metadata, "angle", transition_metadata.angle);
  any_cast_set(metadata, "forward", transition_metadata.forward);
  any_cast_set(metadata, "wipetype", transition_metadata.wipetype);

  return transition_metadata;
}

static void set_strip_metadata_sound(AnyDictionary &metadata, Strip *strip)
{
  AnyDictionary sound;
  if (!any_cast_set(metadata, "sound", sound)) {
    return;
  }

  any_cast_set(sound, "volume", strip->volume);
  any_cast_set(sound, "speed_factor", strip->speed_factor);
  any_cast_set(sound, "pan", strip->pan);
  any_cast_set(sound, "sound_offset", strip->sound_offset);
  any_cast_set_flag(sound, "preserve_pitch", strip->flag, SEQ_AUDIO_PITCH_CORRECTION);
  any_cast_set_flag(sound, "display_waveforms", strip->flag, SEQ_AUDIO_DRAW_WAVEFORM);
  if (strip->sound) {
    any_cast_set_flag(sound, "mono", strip->sound->flags, SOUND_FLAGS_MONO);
  }
}

static void set_strip_metadata_transform(AnyDictionary &metadata, Strip *strip)
{
  AnyDictionary transform_metadata;
  if (!any_cast_set(metadata, "transform", transform_metadata) || !strip->data ||
      !strip->data->transform)
  {
    return;
  }

  StripTransform *transform = strip->data->transform;

  any_cast_set(transform_metadata, "xofs", transform->xofs);
  any_cast_set(transform_metadata, "yofs", transform->yofs);
  any_cast_set(transform_metadata, "scale_x", transform->scale_x);
  any_cast_set(transform_metadata, "scale_y", transform->scale_y);
  any_cast_set(transform_metadata, "rotation", transform->rotation);
  any_cast_set(transform_metadata, "filter", transform->filter);
  any_cast_set_flag(transform_metadata, "flipx", strip->flag, SEQ_FLIPX);
  any_cast_set_flag(transform_metadata, "flipy", strip->flag, SEQ_FLIPY);

  AnyVector origin;
  any_cast_set(transform_metadata, "origin", origin);
  for (size_t i = 0; i < std::min((size_t)2, origin.size()); ++i) {
    any_cast_set(origin[i], transform->origin[i]);
  }
}

static void set_strip_metadata_crop(AnyDictionary &metadata, Strip *strip)
{
  AnyDictionary crop_metadata;
  if (!any_cast_set(metadata, "crop", crop_metadata) || !strip->data || !strip->data->crop) {
    return;
  }

  StripCrop *crop = strip->data->crop;

  any_cast_set(crop_metadata, "top", crop->top);
  any_cast_set(crop_metadata, "bottom", crop->bottom);
  any_cast_set(crop_metadata, "left", crop->left);
  any_cast_set(crop_metadata, "right", crop->right);
}

static void set_strip_metadata_compositing(AnyDictionary &metadata, Strip *strip)
{
  AnyDictionary compositing_metadata;
  if (!any_cast_set(metadata, "compositing", compositing_metadata)) {
    return;
  }

  any_cast_set(compositing_metadata, "blend_mode", strip->blend_mode);
  any_cast_set(compositing_metadata, "blend_opacity", strip->blend_opacity);
}

static void set_strip_metadata_color(AnyDictionary &metadata, Strip *strip)
{
  AnyDictionary color_metadata;
  if (!any_cast_set(metadata, "color", color_metadata)) {
    return;
  }

  any_cast_set(color_metadata, "saturation", strip->sat);
  any_cast_set(color_metadata, "multiply", strip->mul);
  any_cast_set_flag(color_metadata, "multiply_alpha", strip->flag, SEQ_MULTIPLY_ALPHA);
  any_cast_set_flag(color_metadata, "convert_to_float", strip->flag, SEQ_MAKE_FLOAT);
}

void set_strip_metadata(Item *item, Strip *strip)
{
  AnyDictionary metadata;
  if (!any_cast_set(item->metadata(), "blender", metadata)) {
    return;
  }

  if (strip->type == STRIP_TYPE_SOUND) {
    set_strip_metadata_sound(metadata, strip);
    return;
  }

  set_strip_metadata_transform(metadata, strip);
  set_strip_metadata_crop(metadata, strip);
  set_strip_metadata_compositing(metadata, strip);
  set_strip_metadata_color(metadata, strip);
}
}  // namespace blender::io::otio
