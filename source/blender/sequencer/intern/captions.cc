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


// TODO: GD;; Arrange and separate between wrappers and core methods.
namespace blender {

namespace ed::vse {
}  // namespace ed::vse

namespace seq {

/* Core Methods - most aren't exposed in header */

CaptionsChannelData *captions_active_get(Editing *ed) {
  if(ed == nullptr){
    return nullptr;
  }

  SeqTimelineChannel *channel = ed->captions_act_channel;
  if(channel == nullptr) {
    return nullptr;
  }

  return channel->captions_data;
}

static void captions_init_default_style(CaptionsChannelData *captions_data)
{
  // TODO: GD;; Rename data
    TextVars *data = MEM_new<TextVars>("textvars");
    captions_data->style = data;

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

void captions_apply_style_single(CaptionsChannelData *captions_data, Scene *scene, Caption *caption) {
  TextVars *leader_vars = captions_data->style;
  if(leader_vars == nullptr) {
      return;
  }
  
  if(caption->use_custom_style == false) {
    return;
  }

  Strip *strip = caption->strip;
  TextVars *vars = (TextVars *)strip->effectdata;
  if(vars == nullptr) {
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

static void captions_apply_style(CaptionsChannelData *captions_data, Scene *scene)
{   
    for (Caption &caption : captions_data->captions) {
      captions_apply_style_single(captions_data, scene, &caption);
    }
}

static CaptionsChannelData *captions_data_ensure(SeqTimelineChannel *channel) {
  if(channel == nullptr){
    return nullptr;
  }

  CaptionsChannelData *captions_data = channel->captions_data;
  if(captions_data == nullptr){
    /* Maybe should call in a cleaner method? */
    captions_data = MEM_new<CaptionsChannelData>("captionsdata");
  }

  /* Still need that check in case there actually was a captions_data, and then the style could be already existing */
  if(captions_data->style == nullptr) {
      captions_init_default_style(captions_data);
  }

  channel->captions_data = captions_data;

  return captions_data;
}

void captions_set_active_channel(Editing *ed, SeqTimelineChannel *channel) {
  if(ed != nullptr) {
    if(channel == nullptr){
      channel = seq::channel_get_by_index(&ed->channels, 1);
    }
    ed->captions_act_channel = channel;
//    captions_update_active(scene); -> Maybe make it update here? buggy
  }
}

static int compare_strips_start(const void *a, const void *b)
{
    const Caption *caption_a = (Caption *) a;
    const Caption *caption_b = (Caption *) b;

    if (!caption_a->strip || !caption_b->strip) {
        return (!caption_a->strip) - (!caption_b->strip);
    }
    
    return (caption_a->strip->start > caption_b->strip->start) - 
           (caption_a->strip->start < caption_b->strip->start);
}

static ListBaseT<struct Caption> captions_build(Editing *ed, CaptionsChannelData *captions_data, int channel_index)
{
  // TODO: GD;; Make them update the existing ones, and not rebuild them

  ListBaseT<Caption> result = {nullptr, nullptr};
  if(ed == nullptr || captions_data == nullptr){
    return result;
  }

  for (Strip &strip : ed->seqbase) {
    if (strip.channel == channel_index) {
      if (strip.type == STRIP_TYPE_TEXT) {
        Caption *caption = (Caption *)MEM_new_zeroed(sizeof(Caption), "strip ref");
        caption->strip = &strip;
        BLI_addtail(&result, caption);
      }
    }
  }
  
  return result;
}

static void captions_free(CaptionsChannelData *captions_data)
{
  if(captions_data == nullptr){
    return;
  }

  for (Caption &caption : captions_data->captions.items_mutable()) {
    MEM_delete(&caption);
  }
  BLI_listbase_clear(&captions_data->captions);
}

void captions_update(CaptionsChannelData *captions_data, Scene *scene, int channel_index) 
{
  if (captions_data != nullptr) {
    
    /* Free old references */
    captions_free(captions_data);

    captions_data->captions = captions_build(seq::editing_get(scene), captions_data, channel_index);
    
    captions_data->cache_dirty = false;

    BLI_listbase_sort(&captions_data->captions, compare_strips_start);

    if(scene != nullptr){
      captions_apply_style(captions_data, scene);
    }
  }
}

// Wrapper Methods
void captions_apply_style_active(Scene *scene){
  Editing *ed = seq::editing_get(scene);
  if(ed == nullptr){
    return;
  }

  CaptionsChannelData *captions_data = captions_active_get(ed);
  captions_apply_style(captions_data, scene);
}

// TODO: GD;;  Make it call on RNA active change, adn find the right way to do that for the first one (maybe on versioning?)
CaptionsChannelData *captions_active_ensure(Editing *ed){
  if(ed == nullptr){
    return nullptr;
  }

  if (ed->captions_act_channel == nullptr) {
    captions_set_active_channel(ed, nullptr);
  }

  return captions_data_ensure(ed->captions_act_channel);
}

Caption *captions_get_single_by_index(CaptionsChannelData *captions_data, int index){
    if(captions_data == nullptr || index < 0) {
        return nullptr;
    }

    int i = 0;
    for (Caption &caption : captions_data->captions) {
        if(i == index) {
            return &caption;
        }
        i++;
    }
    return nullptr;
}

Caption *captions_get_single_by_strip(CaptionsChannelData *captions_data, struct Strip *strip) {
  if(captions_data == nullptr || strip == nullptr) {
    return nullptr;
  }
  
  for (Caption &caption : captions_data->captions) {
        if (caption.strip == strip) {
            return &caption;
        }
        }
    return nullptr;
}

void captions_mark_caption_style_custom(Caption *caption, bool use_custom) {
    if(caption != nullptr) {
      caption->use_custom_style = use_custom ? 1 : 0;
    }
}

void captions_update_active(Scene *scene) {
  Editing *ed = blender::seq::editing_get(scene);
  
  if (ed == nullptr) {
    return;
  }
  
  if (ed->captions_act_channel == nullptr) {
    captions_set_active_channel(ed, nullptr);
  }

  if (ed->captions_act_channel != nullptr) {
      CaptionsChannelData *captions_data = captions_active_ensure(ed);
      captions_update(captions_data, scene, ed->captions_act_channel->index);
  }
}

/* Can be used without scene just for redraw without update */ //!NOT WORKING AS OF NOW!
void captions_tag_redraw(ARegion *region, Scene *scene)
{
  if(scene != nullptr) {
    captions_update_active(scene);
  }

  ED_region_tag_redraw(region);
}

}  // namespace seq
}  // namespace blender
