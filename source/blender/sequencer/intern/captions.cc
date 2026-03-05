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

void captions_update_active_channel(Editing *ed){
    if (ed != nullptr) {
        ed->captions_act_channel = blender::seq::channel_get_by_index(&ed->channels, 1);
    }
}
}  // namespace ed::vse

namespace seq {

static void captions_init_default_style(Editing *ed)
{
    TextVars *data = MEM_new<TextVars>("textvars");
    ed->captions_style = data;

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

void captions_update_strips_style(Scene *scene)
{
    if(scene == nullptr){
        return;
        
    }

    Editing *ed = seq::editing_get(scene);
    if(ed == nullptr){
        return;
    }

    TextVars *leader_vers = ed->captions_style;
    if(leader_vers == nullptr) {
        return;
    }
    
    for (CaptionsStripRef &ref : ed->captions_strips) {
      if(ref.use_custom_style) {
        continue;
      }

      Strip *strip = ref.strip;
      TextVars *vers = (TextVars *)strip->effectdata;
      if(vers == nullptr) {
        continue;
      }
            /* Font and size */
            vers->text_font = leader_vers->text_font;
            vers->text_size = leader_vers->text_size;

            /* Colors */
            copy_v4_v4(vers->color, leader_vers->color);
            copy_v4_v4(vers->shadow_color, leader_vers->shadow_color);
            copy_v4_v4(vers->outline_color, leader_vers->outline_color);
            copy_v4_v4(vers->box_color, leader_vers->box_color);

            /* Shadow */
            vers->shadow_angle = leader_vers->shadow_angle;
            vers->shadow_offset = leader_vers->shadow_offset;
            vers->shadow_blur = leader_vers->shadow_blur;

            /* Outline */
            vers->outline_width = leader_vers->outline_width;

            copy_v3_v3(vers->loc, leader_vers->loc);
            vers->wrap_width = leader_vers->wrap_width;
            vers->box_margin = leader_vers->box_margin;
            vers->box_roundness = leader_vers->box_roundness;

            vers->align = leader_vers->align;
            vers->anchor_x = leader_vers->anchor_x;
            vers->anchor_y = leader_vers->anchor_y;

            /* All style flags */
            vers->flag = leader_vers->flag;

            seq::relations_invalidate_cache_raw(scene, strip);
        }
    }


    TextVars *captions_style_ensure(Editing *ed) {
    if(ed->captions_style == nullptr) {
        captions_init_default_style(ed);
    }
    if (ed->captions_act_channel == nullptr) {
      ed::vse::captions_update_active_channel(ed);
    }
    return ed->captions_style;
}

CaptionsStripRef *captions_get_ref_by_index(struct Editing *ed, int index){
    if(ed == nullptr || index < 0) {
        return nullptr;
    }

    int i = 0;
    for (CaptionsStripRef &ref : ed->captions_strips) {
        if(i == index) {
            return &ref;
        }
        i++;
    }
    return nullptr;
}


CaptionsStripRef *captions_get_ref_by_strip(Editing *ed, struct Strip *strip) {
    for (CaptionsStripRef &ref : ed->captions_strips) {
        if (ref.strip == strip) {
            return &ref;
        }
        }
    return nullptr;
}

void captions_mark_ref_style_custom(CaptionsStripRef *ref, bool use_custom) {
    if(ref != nullptr) {
        ref->use_custom_style = use_custom ? 1 : 0;
    }
}

static ListBaseT<struct CaptionsStripRef> captions_build_strip_refs(Editing *ed)
{
  ListBaseT<CaptionsStripRef> result = {nullptr, nullptr};
  if(ed == nullptr){
    return result;
  }

  for (Strip &strip : ed->seqbase) {
    if (strip.channel == ed->captions_act_channel->index) {
      if (strip.type == STRIP_TYPE_TEXT) {
        CaptionsStripRef *ref = (CaptionsStripRef *)MEM_new_zeroed(sizeof(CaptionsStripRef), "strip ref");
        ref->strip = &strip;
        BLI_addtail(&result, ref);
      }
    }
  }
  
  return result;
}

static void captions_free_strip_refs(Editing *ed)
{
  if(ed == nullptr){
    return;
  }

  ListBase *refs = &ed->captions_strips;
  for (CaptionsStripRef &ref : ed->captions_strips.items_mutable()) {
    MEM_delete(&ref);
  }
  BLI_listbase_clear(refs);
}

static int compare_strips_start(const void *a, const void *b)
{
    const CaptionsStripRef *ref_a = (CaptionsStripRef *) a;
    const CaptionsStripRef *ref_b = (CaptionsStripRef *) b;
    
    if (!ref_a->strip || !ref_b->strip) {
        return (!ref_a->strip) - (!ref_b->strip);
    }
    
    return (ref_a->strip->start > ref_b->strip->start) - 
           (ref_a->strip->start < ref_b->strip->start);
}

void captions_update_strips(Scene *scene) 
{
  Editing *ed = blender::seq::editing_get(scene);
  
  if (ed == nullptr) {
    return;
  }
  
  if (ed->captions_act_channel == nullptr) {
    ed::vse::captions_update_active_channel(ed);
  }
   
  if (ed->captions_act_channel != nullptr) {
    
    /* Free old references */
    captions_free_strip_refs(ed);

    ed->captions_strips = captions_build_strip_refs(ed);
    
    ed->captions_cache_dirty =  false;

    BLI_listbase_sort(&ed->captions_strips, compare_strips_start);

    captions_update_strips_style(scene);
  }
}

/* Can be used without scene just for redraw without update */ //!NOT WORKING AS OF NOW!
void captions_tag_redraw(ARegion *region, Scene *scene)
{
  if(scene != nullptr) {
    captions_update_strips(scene);
  }

  ED_region_tag_redraw(region);
}

}  // namespace seq
}  // namespace blender
