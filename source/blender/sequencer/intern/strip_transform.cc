/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2003-2009 Blender Authors
 * SPDX-FileCopyrightText: 2005-2006 Peter Schlaile <peter [at] schlaile [dot] de>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "BLI_bounds.hh"
#include "BLI_listbase.h"
#include "BLI_math_base.h"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"

#include "BKE_scene.hh"

#include "BLF_api.hh"

#include "SEQ_animation.hh"
#include "SEQ_channels.hh"
#include "SEQ_edit.hh"
#include "SEQ_effects.hh"
#include "SEQ_iterator.hh"
#include "SEQ_relations.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_time.hh"
#include "SEQ_transform.hh"

#include "effects/effects.hh"
#include "sequencer.hh"
#include "strip_time.hh"

namespace blender::seq {

bool transform_single_image_check(const Strip *strip)
{
  return (strip->flag & SEQ_SINGLE_FRAME_CONTENT) != 0;
}

bool transform_strip_can_be_translated(const Strip *strip)
{
  return !strip->is_effect() || (effect_get_num_inputs(strip->type) == 0);
}

bool transform_test_overlap(const Scene *scene, Strip *strip1, Strip *strip2)
{
  return (
      strip1 != strip2 && strip1->channel == strip2->channel &&
      ((time_right_handle_frame_get(scene, strip1) <= time_left_handle_frame_get(scene, strip2)) ||
       (time_left_handle_frame_get(scene, strip1) >=
        time_right_handle_frame_get(scene, strip2))) == 0);
}

bool transform_test_overlap(const Scene *scene, ListBase *seqbasep, Strip *test)
{
  Strip *strip;

  strip = static_cast<Strip *>(seqbasep->first);
  while (strip) {
    if (transform_test_overlap(scene, test, strip)) {
      return true;
    }

    strip = strip->next;
  }
  return false;
}

void transform_translate_strip(Scene *evil_scene, Strip *strip, int delta)
{
  if (delta == 0) {
    return;
  }

  /* Meta strips requires their content is to be translated, and then frame range of the meta is
   * updated based on nested strips. This won't work for empty meta-strips,
   * so they can be treated as normal strip. */
  if (strip->type == STRIP_TYPE_META && !BLI_listbase_is_empty(&strip->seqbase)) {
    LISTBASE_FOREACH (Strip *, strip_child, &strip->seqbase) {
      transform_translate_strip(evil_scene, strip_child, delta);
    }
    /* Move meta start/end points. */
    strip_time_translate_handles(evil_scene, strip, delta);
  }
  else if (strip->input1 == nullptr && strip->input2 == nullptr) { /* All other strip types. */
    strip->start += delta;
    /* Only to make files usable in older versions. */
    strip->startdisp = time_left_handle_frame_get(evil_scene, strip);
    strip->enddisp = time_right_handle_frame_get(evil_scene, strip);
  }

  offset_animdata(evil_scene, strip, delta);
  Span<Strip *> effects = SEQ_lookup_effects_by_strip(evil_scene->ed, strip);
  strip_time_update_effects_strip_range(evil_scene, effects);
  time_update_meta_strip_range(evil_scene, lookup_meta_by_strip(evil_scene->ed, strip));
}

bool transform_seqbase_shuffle_ex(ListBase *seqbasep,
                                  Strip *test,
                                  Scene *evil_scene,
                                  int channel_delta)
{
  const int orig_channel = test->channel;
  BLI_assert(ELEM(channel_delta, -1, 1));

  strip_channel_set(test, test->channel + channel_delta);

  const ListBase *channels = channels_displayed_get(editing_get(evil_scene));
  SeqTimelineChannel *channel = channel_get_by_index(channels, test->channel);

  bool use_fallback_translation = false;

  while (transform_test_overlap(evil_scene, seqbasep, test) || channel_is_muted(channel) ||
         channel_is_locked(channel))
  {
    if ((channel_delta > 0) ? (test->channel + channel_delta >= MAX_CHANNELS) :
                              (test->channel + channel_delta < 1))
    {
      use_fallback_translation = true;
      break;
    }

    strip_channel_set(test, test->channel + channel_delta);
    channel = channel_get_by_index(channels, test->channel);
  }

  /* Strip can not be moved to next free channel, translate it instead. */
  if (use_fallback_translation) {
    int new_frame = time_right_handle_frame_get(evil_scene, test);

    LISTBASE_FOREACH (Strip *, strip, seqbasep) {
      if (strip->channel == orig_channel) {
        new_frame = max_ii(new_frame, time_right_handle_frame_get(evil_scene, strip));
      }
    }

    strip_channel_set(test, orig_channel);

    new_frame = new_frame + (test->start - time_left_handle_frame_get(
                                               evil_scene, test)); /* adjust by the startdisp */
    transform_translate_strip(evil_scene, test, new_frame - test->start);
    return false;
  }

  return true;
}

bool transform_seqbase_shuffle(ListBase *seqbasep, Strip *test, Scene *evil_scene)
{
  return transform_seqbase_shuffle_ex(seqbasep, test, evil_scene, 1);
}

static bool shuffle_strip_test_overlap(const Scene *scene,
                                       const Strip *strip1,
                                       const Strip *strip2,
                                       const int offset)
{
  BLI_assert(strip1 != strip2);
  return (strip1->channel == strip2->channel &&
          ((time_right_handle_frame_get(scene, strip1) + offset <=
            time_left_handle_frame_get(scene, strip2)) ||
           (time_left_handle_frame_get(scene, strip1) + offset >=
            time_right_handle_frame_get(scene, strip2))) == 0);
}

static int shuffle_strip_time_offset_get(const Scene *scene,
                                         Span<Strip *> strips_to_shuffle,
                                         ListBase *seqbasep,
                                         char dir)
{
  int offset = 0;
  bool all_conflicts_resolved = false;

  while (!all_conflicts_resolved) {
    all_conflicts_resolved = true;
    for (Strip *strip : strips_to_shuffle) {
      LISTBASE_FOREACH (Strip *, strip_other, seqbasep) {
        if (strips_to_shuffle.contains(strip_other)) {
          continue;
        }
        if (relation_is_effect_of_strip(strip_other, strip)) {
          continue;
        }
        if (!shuffle_strip_test_overlap(scene, strip, strip_other, offset)) {
          continue;
        }

        all_conflicts_resolved = false;

        if (dir == 'L') {
          offset = min_ii(offset,
                          time_left_handle_frame_get(scene, strip_other) -
                              time_right_handle_frame_get(scene, strip));
        }
        else {
          offset = max_ii(offset,
                          time_right_handle_frame_get(scene, strip_other) -
                              time_left_handle_frame_get(scene, strip));
        }
      }
    }
  }

  return offset;
}

bool transform_seqbase_shuffle_time(Span<Strip *> strips_to_shuffle,
                                    ListBase *seqbasep,
                                    Scene *evil_scene,
                                    ListBase *markers,
                                    const bool use_sync_markers)
{
  VectorSet<Strip *> empty_set;
  return transform_seqbase_shuffle_time(
      strips_to_shuffle, empty_set, seqbasep, evil_scene, markers, use_sync_markers);
}

bool transform_seqbase_shuffle_time(Span<Strip *> strips_to_shuffle,
                                    Span<Strip *> time_dependent_strips,
                                    ListBase *seqbasep,
                                    Scene *evil_scene,
                                    ListBase *markers,
                                    const bool use_sync_markers)
{
  int offset_l = shuffle_strip_time_offset_get(evil_scene, strips_to_shuffle, seqbasep, 'L');
  int offset_r = shuffle_strip_time_offset_get(evil_scene, strips_to_shuffle, seqbasep, 'R');
  int offset = (-offset_l < offset_r) ? offset_l : offset_r;

  if (offset) {
    for (Strip *strip : strips_to_shuffle) {
      transform_translate_strip(evil_scene, strip, offset);
      strip->runtime.flag &= ~STRIP_OVERLAP;
    }

    if (!time_dependent_strips.is_empty()) {
      for (Strip *strip : time_dependent_strips) {
        offset_animdata(evil_scene, strip, offset);
      }
    }

    if (use_sync_markers && !(evil_scene->toolsettings->lock_markers) && (markers != nullptr)) {
      /* affect selected markers - it's unlikely that we will want to affect all in this way? */
      LISTBASE_FOREACH (TimeMarker *, marker, markers) {
        if (marker->flag & SELECT) {
          marker->frame += offset;
        }
      }
    }
  }

  return offset ? false : true;
}

static VectorSet<Strip *> extract_standalone_strips(Span<Strip *> transformed_strips)
{
  VectorSet<Strip *> standalone_strips;

  for (Strip *strip : transformed_strips) {
    if (!strip->is_effect() || strip->input1 == nullptr) {
      standalone_strips.add(strip);
    }
  }
  return standalone_strips;
}

/* Query strips positioned after left edge of transformed strips bound-box. */
static VectorSet<Strip *> query_right_side_strips(const Scene *scene,
                                                  ListBase *seqbase,
                                                  Span<Strip *> transformed_strips,
                                                  Span<Strip *> time_dependent_strips)
{
  int minframe = MAXFRAME;
  {
    for (Strip *strip : transformed_strips) {
      minframe = min_ii(minframe, time_left_handle_frame_get(scene, strip));
    }
  }

  VectorSet<Strip *> right_side_strips;
  LISTBASE_FOREACH (Strip *, strip, seqbase) {
    if (!time_dependent_strips.is_empty() && time_dependent_strips.contains(strip)) {
      continue;
    }
    if (transformed_strips.contains(strip)) {
      continue;
    }

    if ((strip->flag & SELECT) == 0 && time_left_handle_frame_get(scene, strip) >= minframe) {
      right_side_strips.add(strip);
    }
  }
  return right_side_strips;
}

/* Offset all strips positioned after left edge of transformed strips bound-box by amount equal
 * to overlap of transformed strips. */
static void strip_transform_handle_expand_to_fit(Scene *scene,
                                                 ListBase *seqbasep,
                                                 Span<Strip *> transformed_strips,
                                                 Span<Strip *> time_dependent_strips,
                                                 bool use_sync_markers)
{
  ListBase *markers = &scene->markers;

  VectorSet right_side_strips = query_right_side_strips(
      scene, seqbasep, transformed_strips, time_dependent_strips);

  /* Temporarily move right side strips beyond timeline boundary. */
  for (Strip *strip : right_side_strips) {
    strip->channel += MAX_CHANNELS * 2;
  }

  /* Shuffle transformed standalone strips. This is because transformed strips can overlap with
   * strips on left side. */
  VectorSet standalone_strips = extract_standalone_strips(transformed_strips);
  transform_seqbase_shuffle_time(
      standalone_strips, time_dependent_strips, seqbasep, scene, markers, use_sync_markers);

  /* Move temporarily moved strips back to their original place and tag for shuffling. */
  for (Strip *strip : right_side_strips) {
    strip->channel -= MAX_CHANNELS * 2;
  }
  /* Shuffle again to displace strips on right side. Final effect shuffling is done in
   * SEQ_transform_handle_overlap. */
  transform_seqbase_shuffle_time(right_side_strips, seqbasep, scene, markers, use_sync_markers);
}

static VectorSet<Strip *> query_overwrite_targets(const Scene *scene,
                                                  ListBase *seqbasep,
                                                  Span<Strip *> transformed_strips)
{
  VectorSet<Strip *> overwrite_targets = query_unselected_strips(seqbasep);

  /* Effects of transformed strips can be unselected. These must not be included. */
  overwrite_targets.remove_if([&](Strip *strip) { return transformed_strips.contains(strip); });
  overwrite_targets.remove_if([&](Strip *strip) {
    bool does_overlap = false;
    for (Strip *strip_transformed : transformed_strips) {
      if (transform_test_overlap(scene, strip, strip_transformed)) {
        does_overlap = true;
      }
    }

    return !does_overlap;
  });

  return overwrite_targets;
}

enum eOvelapDescrition {
  /* No overlap. */
  STRIP_OVERLAP_NONE,
  /* Overlapping strip covers overlapped completely. */
  STRIP_OVERLAP_IS_FULL,
  /* Overlapping strip is inside overlapped. */
  STRIP_OVERLAP_IS_INSIDE,
  /* Partial overlap between 2 strips. */
  STRIP_OVERLAP_LEFT_SIDE,
  STRIP_OVERLAP_RIGHT_SIDE,
};

static eOvelapDescrition overlap_description_get(const Scene *scene,
                                                 const Strip *transformed,
                                                 const Strip *target)
{
  if (time_left_handle_frame_get(scene, transformed) <=
          time_left_handle_frame_get(scene, target) &&
      time_right_handle_frame_get(scene, transformed) >=
          time_right_handle_frame_get(scene, target))
  {
    return STRIP_OVERLAP_IS_FULL;
  }
  if (time_left_handle_frame_get(scene, transformed) > time_left_handle_frame_get(scene, target) &&
      time_right_handle_frame_get(scene, transformed) < time_right_handle_frame_get(scene, target))
  {
    return STRIP_OVERLAP_IS_INSIDE;
  }
  if (time_left_handle_frame_get(scene, transformed) <=
          time_left_handle_frame_get(scene, target) &&
      time_left_handle_frame_get(scene, target) <= time_right_handle_frame_get(scene, transformed))
  {
    return STRIP_OVERLAP_LEFT_SIDE;
  }
  if (time_left_handle_frame_get(scene, transformed) <=
          time_right_handle_frame_get(scene, target) &&
      time_right_handle_frame_get(scene, target) <=
          time_right_handle_frame_get(scene, transformed))
  {
    return STRIP_OVERLAP_RIGHT_SIDE;
  }
  return STRIP_OVERLAP_NONE;
}

/* Split strip in 3 parts, remove middle part and fit transformed inside. */
static void strip_transform_handle_overwrite_split(Scene *scene,
                                                   ListBase *seqbasep,
                                                   const Strip *transformed,
                                                   Strip *target)
{
  /* Because we are doing a soft split, bmain is not used in SEQ_edit_strip_split, so we can
   * pass nullptr here. */
  Main *bmain = nullptr;
  const char *error_msg = nullptr;
  Strip *split_strip = edit_strip_split(bmain,
                                        scene,
                                        seqbasep,
                                        target,
                                        time_left_handle_frame_get(scene, transformed),
                                        SPLIT_SOFT,
                                        true,
                                        &error_msg);
  if (split_strip == nullptr) {
    return;
  }

  error_msg = nullptr;
  if (edit_strip_split(bmain,
                       scene,
                       seqbasep,
                       split_strip,
                       time_right_handle_frame_get(scene, transformed),
                       SPLIT_SOFT,
                       true,
                       &error_msg) == nullptr)
  {
    return;
  }
  edit_flag_for_removal(scene, seqbasep, split_strip);
  edit_remove_flagged_strips(scene, seqbasep);
}

/* Trim strips by adjusting handle position.
 * This is bit more complicated in case overlap happens on effect. */
static void strip_transform_handle_overwrite_trim(Scene *scene,
                                                  ListBase *seqbasep,
                                                  const Strip *transformed,
                                                  Strip *target,
                                                  const eOvelapDescrition overlap)
{
  VectorSet targets = query_by_reference(target, scene, seqbasep, query_strip_effect_chain);

  /* Expand collection by adding all target's children, effects and their children. */
  if (target->is_effect()) {
    iterator_set_expand(scene, seqbasep, targets, query_strip_effect_chain);
  }

  /* Trim all non effects, that have influence on effect length which is overlapping. */
  for (Strip *strip : targets) {
    if (strip->is_effect() && effect_get_num_inputs(strip->type) > 0) {
      continue;
    }
    if (overlap == STRIP_OVERLAP_LEFT_SIDE) {
      time_left_handle_frame_set(scene, strip, time_right_handle_frame_get(scene, transformed));
    }
    else {
      BLI_assert(overlap == STRIP_OVERLAP_RIGHT_SIDE);
      time_right_handle_frame_set(scene, strip, time_left_handle_frame_get(scene, transformed));
    }
  }
}

static void strip_transform_handle_overwrite(Scene *scene,
                                             ListBase *seqbasep,
                                             Span<Strip *> transformed_strips)
{
  VectorSet targets = query_overwrite_targets(scene, seqbasep, transformed_strips);
  VectorSet<Strip *> strips_to_delete;

  ListBase *channels = channels_displayed_get(editing_get(scene));
  for (Strip *target : targets) {
    for (Strip *transformed : transformed_strips) {
      if (transformed->channel != target->channel) {
        continue;
      }
      /* Do not allow overwriting/trimming/deleting locked strips. */
      if (transform_is_locked(channels, target)) {
        continue;
      }

      const eOvelapDescrition overlap = overlap_description_get(scene, transformed, target);

      if (overlap == STRIP_OVERLAP_IS_FULL) {
        strips_to_delete.add(target);
      }
      else if (overlap == STRIP_OVERLAP_IS_INSIDE) {
        strip_transform_handle_overwrite_split(scene, seqbasep, transformed, target);
      }
      else if (ELEM(overlap, STRIP_OVERLAP_LEFT_SIDE, STRIP_OVERLAP_RIGHT_SIDE)) {
        strip_transform_handle_overwrite_trim(scene, seqbasep, transformed, target, overlap);
      }
    }
  }

  /* Remove covered strips. This must be done in separate loop, because
   * `SEQ_edit_strip_split()` also uses `SEQ_edit_remove_flagged_sequences()`. See #91096. */
  if (!strips_to_delete.is_empty()) {
    for (Strip *strip : strips_to_delete) {
      edit_flag_for_removal(scene, seqbasep, strip);
    }
    edit_remove_flagged_strips(scene, seqbasep);
  }
}

static void strip_transform_handle_overlap_shuffle(Scene *scene,
                                                   ListBase *seqbasep,
                                                   Span<Strip *> transformed_strips,
                                                   Span<Strip *> time_dependent_strips,
                                                   bool use_sync_markers)
{
  ListBase *markers = &scene->markers;

  /* Shuffle non strips with no effects attached. */
  VectorSet standalone_strips = extract_standalone_strips(transformed_strips);
  transform_seqbase_shuffle_time(
      standalone_strips, time_dependent_strips, seqbasep, scene, markers, use_sync_markers);
}

void transform_handle_overlap(Scene *scene,
                              ListBase *seqbasep,
                              Span<Strip *> transformed_strips,
                              bool use_sync_markers)
{
  VectorSet<Strip *> empty_set;
  transform_handle_overlap(scene, seqbasep, transformed_strips, empty_set, use_sync_markers);
}

void transform_handle_overlap(Scene *scene,
                              ListBase *seqbasep,
                              Span<Strip *> transformed_strips,
                              Span<Strip *> time_dependent_strips,
                              bool use_sync_markers)
{
  const eSeqOverlapMode overlap_mode = tool_settings_overlap_mode_get(scene);

  switch (overlap_mode) {
    case SEQ_OVERLAP_EXPAND:
      strip_transform_handle_expand_to_fit(
          scene, seqbasep, transformed_strips, time_dependent_strips, use_sync_markers);
      break;
    case SEQ_OVERLAP_OVERWRITE:
      strip_transform_handle_overwrite(scene, seqbasep, transformed_strips);
      break;
    case SEQ_OVERLAP_SHUFFLE:
      strip_transform_handle_overlap_shuffle(
          scene, seqbasep, transformed_strips, time_dependent_strips, use_sync_markers);
      break;
  }

  /* If any effects still overlap, we need to move them up.
   * In some cases other strips can be overlapping still, see #90646. */
  for (Strip *strip : transformed_strips) {
    if (transform_test_overlap(scene, seqbasep, strip)) {
      transform_seqbase_shuffle(seqbasep, strip, scene);
    }
    strip->runtime.flag &= ~STRIP_OVERLAP;
  }
}

void transform_offset_after_frame(Scene *scene,
                                  ListBase *seqbase,
                                  const int delta,
                                  const int timeline_frame)
{
  LISTBASE_FOREACH (Strip *, strip, seqbase) {
    if (time_left_handle_frame_get(scene, strip) >= timeline_frame) {
      transform_translate_strip(scene, strip, delta);
      relations_invalidate_cache(scene, strip);
    }
  }

  if (!scene->toolsettings->lock_markers) {
    LISTBASE_FOREACH (TimeMarker *, marker, &scene->markers) {
      if (marker->frame >= timeline_frame) {
        marker->frame += delta;
      }
    }
  }
}

void strip_channel_set(Strip *strip, int channel)
{
  strip->channel = math::clamp(channel, 1, MAX_CHANNELS);
}

bool transform_is_locked(ListBase *channels, const Strip *strip)
{
  const SeqTimelineChannel *channel = channel_get_by_index(channels, strip->channel);
  return strip->flag & SEQ_LOCK ||
         (channel_is_locked(channel) && ((strip->runtime.flag & STRIP_IGNORE_CHANNEL_LOCK) == 0));
}

float2 image_transform_mirror_factor_get(const Strip *strip)
{
  float2 mirror(1.0f, 1.0f);

  if ((strip->flag & SEQ_FLIPX) != 0) {
    mirror.x = -1.0f;
  }
  if ((strip->flag & SEQ_FLIPY) != 0) {
    mirror.y = -1.0f;
  }
  return mirror;
}

float2 transform_image_raw_size_get(const Scene *scene, const Strip *strip)
{
  float2 scene_render_size(scene->r.xsch, scene->r.ysch);

  if (ELEM(strip->type, STRIP_TYPE_MOVIE, STRIP_TYPE_IMAGE)) {
    const StripElem *selem = strip->data->stripdata;
    return {float(selem->orig_width), float(selem->orig_height)};
  }

  if (strip->type == STRIP_TYPE_MOVIECLIP) {
    const MovieClip *clip = strip->clip;
    if (clip != nullptr && clip->lastsize[0] != 0 && clip->lastsize[1] != 0) {
      return {float(clip->lastsize[0]), float(clip->lastsize[1])};
    }
  }

  if (strip->type == STRIP_TYPE_TEXT) {
    const TextVars *data = static_cast<TextVars *>(strip->effectdata);
    const FontFlags font_flags = ((data->flag & SEQ_TEXT_BOLD) ? BLF_BOLD : BLF_NONE) |
                                 ((data->flag & SEQ_TEXT_ITALIC) ? BLF_ITALIC : BLF_NONE);
    const int font = text_effect_font_init(nullptr, strip, font_flags);
    const TextVarsRuntime *runtime = text_effect_calc_runtime(
        strip, font, int2(scene_render_size));
    BLF_disable(font, font_flags);

    const float2 text_size(float(BLI_rcti_size_x(&runtime->text_boundbox)),
                           float(BLI_rcti_size_y(&runtime->text_boundbox)));
    MEM_delete(runtime);
    return text_size;
  }

  return scene_render_size;
}

float2 image_transform_origin_get(const Scene *scene, const Strip *strip)
{

  const StripTransform *transform = strip->data->transform;
  if (strip->type != STRIP_TYPE_TEXT) {
    return {transform->origin[0], transform->origin[1]};
  }

  /* Text image size is different from true image size, so the origin position must be
   * calculated. */
  float2 scene_render_size(scene->r.xsch, scene->r.ysch);
  const float2 text_image_size = transform_image_raw_size_get(scene, strip);
  const float2 scale = text_image_size / scene_render_size;
  const float2 origin_rel(transform->origin[0], transform->origin[1]);
  const float2 origin_center(0.5f, 0.5f);
  const float2 origin_diff = origin_rel - origin_center;

  const float2 true_origin_relative = origin_center + origin_diff * scale;
  return true_origin_relative;
}

float2 image_transform_origin_offset_pixelspace_get(const Scene *scene, const Strip *strip)
{
  const StripTransform *transform = strip->data->transform;
  const float2 image_size = transform_image_raw_size_get(scene, strip);
  const float2 origin_relative(transform->origin[0], transform->origin[1]);
  const float2 translation(transform->xofs, transform->yofs);
  const float2 origin_pos_pixels = (image_size * origin_relative) - (image_size * 0.5f) +
                                   translation;
  const float2 viewport_pixel_aspect(scene->r.xasp / scene->r.yasp, 1.0f);
  const float2 mirror = image_transform_mirror_factor_get(strip);
  return origin_pos_pixels * mirror * viewport_pixel_aspect;
}

static float3x3 seq_image_transform_matrix_get_ex(const Scene *scene,
                                                  const Strip *strip,
                                                  bool apply_rotation = true)
{
  const StripTransform *transform = strip->data->transform;
  const float2 image_size = transform_image_raw_size_get(scene, strip);
  const float2 origin_relative(transform->origin[0], transform->origin[1]);
  const float2 origin_absolute = image_size * origin_relative;
  const float2 translation(transform->xofs, transform->yofs);
  const float rotation = apply_rotation ? transform->rotation : 0.0f;
  const float2 scale(transform->scale_x, transform->scale_y);
  const float2 pivot = origin_absolute - (image_size / 2);

  const float3x3 matrix = math::from_loc_rot_scale<float3x3>(translation, rotation, scale);
  return math::from_origin_transform(matrix, pivot);
}

float3x3 image_transform_matrix_get(const Scene *scene, const Strip *strip)
{
  return seq_image_transform_matrix_get_ex(scene, strip);
}

static Array<float2> strip_image_transform_quad_get_ex(const Scene *scene,
                                                       const Strip *strip,
                                                       bool apply_rotation)
{
  const float2 image_size = transform_image_raw_size_get(scene, strip);

  const StripCrop *crop = strip->data->crop;
  float2 quad[4]{
      {(image_size[0] / 2) - crop->right, (image_size[1] / 2) - crop->top},
      {(image_size[0] / 2) - crop->right, (-image_size[1] / 2) + crop->bottom},
      {(-image_size[0] / 2) + crop->left, (-image_size[1] / 2) + crop->bottom},
      {(-image_size[0] / 2) + crop->left, (image_size[1] / 2) - crop->top},
  };

  const float3x3 matrix = seq_image_transform_matrix_get_ex(scene, strip, apply_rotation);
  const float2 viewport_pixel_aspect(scene->r.xasp / scene->r.yasp, 1.0f);
  const float2 mirror = image_transform_mirror_factor_get(strip);

  Array<float2> quad_transformed;
  quad_transformed.reinitialize(4);

  for (int i = 0; i < 4; i++) {
    const float2 point = math::transform_point(matrix, quad[i]);
    quad_transformed[i] = point * mirror * viewport_pixel_aspect;
  }
  return quad_transformed;
}

Array<float2> image_transform_quad_get(const Scene *scene, const Strip *strip, bool apply_rotation)
{
  return strip_image_transform_quad_get_ex(scene, strip, apply_rotation);
}

Array<float2> image_transform_final_quad_get(const Scene *scene, const Strip *strip)
{
  return strip_image_transform_quad_get_ex(scene, strip, true);
}

float2 image_preview_unit_to_px(const Scene *scene, const float2 co_src)
{
  return {co_src.x * scene->r.xsch, co_src.y * scene->r.ysch};
}

float2 image_preview_unit_from_px(const Scene *scene, const float2 co_src)
{
  return {co_src.x / scene->r.xsch, co_src.y / scene->r.ysch};
}

static Bounds<float2> negative_bounds()
{
  return {float2(std::numeric_limits<float>::max()), float2(std::numeric_limits<float>::lowest())};
}

Bounds<float2> image_transform_bounding_box_from_collection(Scene *scene,
                                                            Span<Strip *> strips,
                                                            bool apply_rotation)
{
  Bounds<float2> box = negative_bounds();

  for (Strip *strip : strips) {
    const Array<float2> quad = image_transform_quad_get(scene, strip, apply_rotation);
    const Bounds<float2> strip_box = *bounds::min_max(quad.as_span());
    box = bounds::merge(box, strip_box);
  }

  return box;
}

bool GapRemover::can_merge_ranges_y(const rcti &range_l, const rcti &range_r)
{
  const bool has_same_time_range = (range_r.xmin >= range_l.xmin &&
                                    range_r.xmax <= range_l.xmax) ||
                                   (range_l.xmin >= range_r.xmin && range_l.xmax <= range_r.xmax);
  return has_same_time_range;
}

bool GapRemover::can_merge_ranges_x(const rcti &range_l, const rcti &range_r)
{
  return range_r.xmin == range_l.xmax || range_r.xmax == range_l.xmin;
}

/* Make as big continuos rectangle as possible. This function accepts discontinuities in Y axis,
 * but it is precise in X axis. Gap Y range is irrelevant for gap removal operation at this point
 * and this works well for purpose of merging these ranges. */
Vector<rcti> GapRemover::unify_gap_ranges(const Vector<rcti> &ranges)
{
  Vector<rcti> unified_ranges_y;

  for (const rcti &range : ranges) {
    bool was_merged = false;
    for (rcti &unified_range : unified_ranges_y) {
      if (this->can_merge_ranges_y(unified_range, range)) {
        unified_range.xmin = math::min(unified_range.xmin, range.xmin);
        unified_range.xmax = math::max(unified_range.xmax, range.xmax);
        unified_range.ymin = math::min(unified_range.ymin, range.ymin);
        unified_range.ymax = math::max(unified_range.ymax, range.ymin);
        was_merged = true;
      }
    }

    if (!was_merged) {
      unified_ranges_y.append({range.xmin, range.xmax, range.ymin, range.ymax});
    }
  }

  std::sort(unified_ranges_y.begin(), unified_ranges_y.end(), [](const rcti &a, const rcti &b) {
    return a.xmin < b.xmin;
  });

  Vector<rcti> unified_ranges_xy;

  for (const rcti &range : unified_ranges_y) {
    bool was_merged = false;
    for (rcti &unified_range : unified_ranges_xy) {
      if (this->can_merge_ranges_x(unified_range, range)) {
        unified_range.xmin = math::min(unified_range.xmin, range.xmin);
        unified_range.xmax = math::max(unified_range.xmax, range.xmax);
        unified_range.ymin = math::min(unified_range.ymin, range.ymin);
        unified_range.ymax = math::max(unified_range.ymax, range.ymin);
        was_merged = true;
      }
    }

    if (!was_merged) {
      unified_ranges_xy.append({range.xmin, range.xmax, range.ymin, range.ymax});
    }
  }
  return unified_ranges_xy;
}

bool GapRemover::strip_intersects_range(const Strip *strip, const rcti gap_range)
{
  rcti strip_range = {time_left_handle_frame_get(scene, strip) + 1,
                      time_right_handle_frame_get(scene, strip) - 1,
                      strip->channel,
                      strip->channel};

  return BLI_rcti_isect(&strip_range, &gap_range, nullptr) ||
         BLI_rcti_inside_rcti(&strip_range, &gap_range);
}

bool GapRemover::gap_is_valid(const rcti &range)
{
  int range_size_x = BLI_rcti_size_x(&range);

  LISTBASE_FOREACH (Strip *, strip, active_seqbase_get(editing_get(scene))) {
    rcti strip_range = {time_left_handle_frame_get(scene, strip),
                        time_right_handle_frame_get(scene, strip),
                        strip->channel,
                        strip->channel};

    /* Subtract the size of strips in the gap from the gap. This is needed when moving strip
     * handles. Only 1 channel is checked, otherwise this would not work. */
    if (strip->channel == range.ymin) {
      rcti range_isect;
      BLI_rcti_isect(&strip_range, &range, &range_isect);
      range_size_x -= BLI_rcti_size_x(&range_isect);
    }

    eWhichStripsCanBeMoved which = tool_settings_gap_removal_mode_get(scene);
    if (which == SEQ_GAPS_MOVE_ABOVE && strip->channel < range.ymin) {
      continue;
    }
    if (which == SEQ_GAPS_MOVE_BELOW && strip->channel > range.ymax) {
      continue;
    }

    /* If any strip is covering the gap exactly, the gap can not be removed. */
    if (strip_range.xmin == range.xmin && strip_range.xmax == range.xmax) {
      return false;
    }
  }
  return range_size_x > 0;
}

bool GapRemover::gap_has_strips_on_both_sides(const rcti &range)
{
  bool has_strips_on_left = false;
  bool has_strips_on_right = false;

  LISTBASE_FOREACH (Strip *, strip, active_seqbase_get(editing_get(scene))) {
    if (strip->channel < range.ymin || strip->channel > range.ymax) {
      continue;
    }

    /* If there are no strips on left and right side of gap, do not remove it. */
    if (time_right_handle_frame_get(scene, strip) == range.xmin) {
      has_strips_on_left = true;
    }
    if (time_left_handle_frame_get(scene, strip) == range.xmax) {
      has_strips_on_right = true;
    }
    if (has_strips_on_left && has_strips_on_right) {
      return true;
    }
  }
  return false;
}

/* Make gaps as large as possible in Y axis and shrink the gap in X axis to fit between strips. */
void GapRemover::optimize_gap_ranges(Vector<rcti> &gap_ranges)
{
  for (rcti &range : gap_ranges) {
    rcti above = {range.xmin, range.xmax, range.ymax + 1, std::numeric_limits<int>::max()};
    rcti below = {range.xmin, range.xmax, 0, range.ymin - 1};

    eWhichStripsCanBeMoved which = tool_settings_gap_removal_mode_get(scene);
    const bool move_above = which == SEQ_GAPS_MOVE_ABOVE || which == SEQ_GAPS_MOVE_ABOVE_AND_BELOW;
    const bool move_below = which == SEQ_GAPS_MOVE_BELOW || which == SEQ_GAPS_MOVE_ABOVE_AND_BELOW;

    /* Start by assuming, that gap is infinite in Y direction; */
    range.ymax = std::numeric_limits<int>::max();
    range.ymin = 0;

    LISTBASE_FOREACH (Strip *, strip, active_seqbase_get(editing_get(scene))) {
      int strip_left = time_left_handle_frame_get(scene, strip);
      int strip_right = time_right_handle_frame_get(scene, strip);

      /* Shrink gap in X axis. */
      if (this->strip_intersects_range(strip, range)) {
        if (strip_left <= range.xmin && strip_right < range.xmax) {
          above.xmin = below.xmin = range.xmin = strip_right;
        }
        else if (strip_left > range.xmin && strip_right >= range.xmax) {
          above.xmax = below.xmax = range.xmax = strip_left;
        }
        else if (strip_left > range.xmin && strip_right < range.xmax) {
          /* The gap could be split, and new one appended here. */
          above.xmin = below.xmin = range.xmin = strip_right;
        }
      }
      /* Shrink gap in Y axis. */
      if (move_below && this->strip_intersects_range(strip, above)) {
        range.ymax = math::min(range.ymax, strip->channel - 1);
      }
      if (move_above && this->strip_intersects_range(strip, below)) {
        range.ymin = math::max(range.ymin, strip->channel + 1);
      }
    }
  }
}

void GapRemover::query_right_side_strips(const rcti gap_range)
{
  rcti right_side_range = gap_range;
  right_side_range.xmax = std::numeric_limits<int>::max();
  LISTBASE_FOREACH (Strip *, strip, active_seqbase_get(editing_get(scene))) {
    if (transform_is_locked(channels, strip)) {
      continue;
    }

    const int left_handle = time_left_handle_frame_get(scene, strip);
    const int right_handle = time_right_handle_frame_get(scene, strip);
    /* Strip range is shrunk, because of how BLI_rcti_ function calculate intersection. */
    rcti strip_range = {left_handle + 1, right_handle - 1, strip->channel, strip->channel};

    /* Gather strips that will be offset. */
    if (BLI_rcti_inside_rcti(&right_side_range, &strip_range) && left_handle != gap_range.xmin) {
      right_side_strips.add(strip);
    }
    /* Gather strips that will have handle moved. */
    else if (BLI_rcti_isect_pt(&right_side_range, right_handle - 1, strip->channel)) {
      right_side_handles.add(strip);
    }
  }
}

void GapRemover::remove_gaps()
{
  eWhichStripsCanBeMoved which = tool_settings_gap_removal_mode_get(scene);

  Vector<rcti> final_gap_ranges = gap_ranges;
  if (which != SEQ_GAPS_MOVE_IN_RANGE) {
    final_gap_ranges = this->unify_gap_ranges(final_gap_ranges);
    final_gap_ranges.remove_if([&](const rcti &range) { return !this->gap_is_valid(range); });
    this->optimize_gap_ranges(final_gap_ranges);
    /* Final validation after the final range size is known. */
    final_gap_ranges.remove_if([&](const rcti &range) {
      return !BLI_rcti_is_valid(&range) || !this->gap_has_strips_on_both_sides(range);
    });
  }

  for (const rcti &range : final_gap_ranges) {
    const int offset = BLI_rcti_size_x(&range);
    this->query_right_side_strips(range);

    for (Strip *strip : right_side_strips) {
      transform_translate_strip(scene, strip, -offset);
      relations_invalidate_cache(scene, strip);
    }
    for (Strip *strip : right_side_handles) {
      const int right_handle_frame = time_right_handle_frame_get(scene, strip);
      time_right_handle_frame_set(scene, strip, right_handle_frame - offset);
      relations_invalidate_cache(scene, strip);
    }
  }

  if (final_gap_ranges.is_empty()) {
    return;
  }

  /* Offset playhead. */
  const int cfra = BKE_scene_frame_get(scene);
  const rcti &range = final_gap_ranges.first();
  if (cfra >= range.xmax) {
    BKE_scene_frame_set(scene, cfra - BLI_rcti_size_x(&range));
  }
  if (cfra > range.xmin && cfra < range.xmax) {
    BKE_scene_frame_set(scene, range.xmin);
  }
}

GapRemover::GapRemover(Scene *scene, VectorSet<Strip *> moved_strips)
    : scene(scene), moved_strips(moved_strips)
{
  Editing *ed = editing_get(scene);
  BLI_assert(ed != nullptr);
  channels = channels_displayed_get(ed);

  /* Only remove gaps for select strip types. With this content, intentions are predictable. */
  eWhichStripsCanBeMoved which = tool_settings_gap_removal_mode_get(scene);

  if (which != SEQ_GAPS_MOVE_IN_RANGE) {
    moved_strips.remove_if([](Strip *strip) {
      return !ELEM(
          strip->type, STRIP_TYPE_MOVIE, STRIP_TYPE_IMAGE, STRIP_TYPE_SCENE, STRIP_TYPE_META);
    });
  }

  for (Strip *strip : moved_strips) {
    gap_ranges.append({time_left_handle_frame_get(scene, strip),
                       time_right_handle_frame_get(scene, strip),
                       strip->channel,
                       strip->channel});
  }
}

}  // namespace blender::seq
