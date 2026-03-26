/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include "DNA_space_types.h"
#include "DNA_object_types.h"
#include "DNA_sequence_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_listBase.h"
#include "BLI_string_utf8.h"
#include "BLI_threads.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"

#include "BKE_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface.hh"
#include "UI_view2d.hh"

#include "SEQ_channels.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_select.hh"
#include "SEQ_relations.hh"
#include "SEQ_captions.hh"

#include "ED_screen.hh"
#include "BLO_read_write.hh"


namespace blender {

namespace ed::vse {
}  // namespace ed::vse

namespace seq {


static void captions_init_default_style(SeqTimelineChannel *channel)
{
    TextVars *data = MEM_new<TextVars>("textvars");
    channel->captions_style = data;

    data->flag |= SEQ_TEXT_OUTLINE;
    data->flag |= SEQ_TEXT_SHADOW;

    data->text_font = nullptr;
    data->text_blf_id = -1;
    data->text_size = 60.0f;

    copy_v4_fl(data->color, 1.0f);
    data->shadow_color[3] = 0.7f;
    data->shadow_angle = DEG2RADF(65.0f);
    data->shadow_offset = 0.04f;
    data->shadow_blur = 0.0f;
    data->box_color[0] = 0.2f;
    data->box_color[1] = 0.2f;
    data->box_color[2] = 0.2f;
    data->box_color[3] = 0.7f;
    data->box_margin = 0.01f;
    data->box_roundness = 0.0f;
    data->outline_color[3] = 0.7f;
    data->outline_width = 0.05f;

    data->loc[0] = 0.5f;
    data->loc[1] = 0.15f;
    data->anchor_x = SEQ_TEXT_ALIGN_X_CENTER;
    data->anchor_y = SEQ_TEXT_ALIGN_Y_CENTER;
    data->align = SEQ_TEXT_ALIGN_X_CENTER;
    data->wrap_width = 1.0f;
}

static TextVars *captions_active_style_get(Scene *scene){
  Editing *ed = seq::editing_get(scene);
  if(ed == nullptr){
    return nullptr;
  }

  return ed->captions_act_channel->captions_style;
}

void captions_apply_style_single(Scene *scene, SeqTimelineChannel *channel, Strip *strip)
{
  TextVars *leader_vars = channel->captions_style;
  if(leader_vars == nullptr) {
      return;
  }
  
  TextVars *vars = (TextVars *)strip->effectdata;
  if(vars == nullptr) {
    return;
  }

  if(vars->captions_use_custom_style) {
    return;
  }


        /* Font and size */
        vars->text_font = leader_vars->text_font;
        vars->text_size = leader_vars->text_size;

        /* Colors */
        copy_v4_v4(vars->color, leader_vars->color);
        copy_v4_v4(vars->shadow_color, leader_vars->shadow_color);
        copy_v4_v4(vars->outline_color, leader_vars->outline_color);
        copy_v4_v4(vars->box_color, leader_vars->box_color);

        /* Shadow */
        vars->shadow_angle = leader_vars->shadow_angle;
        vars->shadow_offset = leader_vars->shadow_offset;
        vars->shadow_blur = leader_vars->shadow_blur;

        /* Outline */
        vars->outline_width = leader_vars->outline_width;

        copy_v3_v3(vars->loc, leader_vars->loc);
        vars->wrap_width = leader_vars->wrap_width;
        vars->box_margin = leader_vars->box_margin;
        vars->box_roundness = leader_vars->box_roundness;

        vars->align = leader_vars->align;
        vars->anchor_x = leader_vars->anchor_x;
        vars->anchor_y = leader_vars->anchor_y;

        /* All style flags */
        vars->flag = leader_vars->flag;

        if(scene != nullptr){
          seq::relations_invalidate_cache_raw(scene, strip);
        }
}

void captions_apply_style_active(Scene *scene)
{ 
    Editing *ed = seq::editing_get(scene);
    if(ed == nullptr){
      return;
    }

    for (Strip *strip : captions_cache_query(scene)) {
      captions_apply_style_single(scene, ed->captions_act_channel, strip);
    }
}

void captions_active_channel_set(Editing *ed, SeqTimelineChannel *channel) {
  if(ed == nullptr) {
    return;
  }
  
  if(channel == nullptr){
    channel = seq::channel_get_by_index(&ed->channels, 1);
  }

  if(channel->captions_style == nullptr) {
    captions_init_default_style(channel);
  }

  ed->captions_act_channel = channel;
}

const Vector<Strip *> captions_cache_query(Scene *scene) {
  Editing *ed = seq::editing_get(scene);
  if(ed == nullptr){
    return {};
  }

  return ed->runtime->captions_cache;
}

Strip *captions_cache_query_index(Scene *scene, int index) {
  if(scene == nullptr || index < 0) {
      return nullptr;
  }

  int i = 0;
  for (Strip *strip : captions_cache_query(scene)) {
      if(i == index) {
          return strip;
      }
      i++;
  }
  return nullptr;
}

void captions_cache_sort(Scene *scene){
  Editing *ed = seq::editing_get(scene);
  std::sort(ed->runtime->captions_cache.begin(), 
  ed->runtime->captions_cache.end(), 
  [](const Strip *a, const Strip *b) {
      if (!a || !b) {
          return a != nullptr; 
      }

      return a->start < b->start;
  });
}

void captions_cache_append(Scene *scene, Strip *strip){
  Editing *ed = seq::editing_get(scene);
  ed->runtime->captions_cache.append(strip);
  captions_cache_sort(scene);
}

void captions_cache_remove(Scene *scene, Strip *strip){
  Editing *ed = seq::editing_get(scene);

  Vector<Strip *> &cache = ed->runtime->captions_cache;
  const int64_t index = cache.first_index_of(strip);
  cache.remove(index);
}

void captions_cache_rebuild(Scene *scene)
{
  Editing *ed = seq::editing_get(scene);
  if(ed == nullptr){
    return;
  }

  if (ed->captions_act_channel == nullptr) {
    captions_active_channel_set(ed, nullptr);
  }

  ed->runtime->captions_cache.clear();
  
  for (Strip &strip : ed->seqbase) {
    if (strip.channel == ed->captions_act_channel->index) {
      if (strip.type == STRIP_TYPE_TEXT) {
        ed->runtime->captions_cache.append(&strip);
      }
    }
  }

  captions_cache_sort(scene);
}

void captions_update_active(Scene *scene){
  captions_cache_rebuild(scene);
  
  if(scene != nullptr){
    captions_apply_style_active(scene);
  }
}

/*static void captions_free(CaptionsChannelData *captions_data)
{
  if(captions_data == nullptr){
    return;
  }

  for (Caption &caption : captions_data->captions.items_mutable()) {
    MEM_delete(&caption);
  }
  BLI_listbase_clear(&captions_data->captions);
}*/ 

void captions_set_style_custom(Strip *strip, bool use_custom) {
    if(strip != nullptr) {
      TextVars *vars = (TextVars *)strip->effectdata;
      vars->captions_use_custom_style = use_custom ? 1 : 0;
    }
}

}  // namespace seq
}  // namespace blender
