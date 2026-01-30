/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spcaptions
 */

#include "DNA_space_types.h"
#include "DNA_object_types.h"
#include "DNA_sequence_types.h"
#include "DNA_captions_types.h"

#include "captions_intern.hh"

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

#include "ED_screen.hh"
#include "BLO_read_write.hh"

namespace blender {

// Todo: Those helper methods should go captions_edit.cc or a new captions.cc, It'll stay here until the exact location of the captions in the UI is decided.
CaptionsStripRef *style_leader_ref_ensure(Editing *ed) {
  /* Currently, each time anything need acsses for the leader, it calls this method, which is quite heavy (loop through all of the refs each time.) */
  if(ed == nullptr){
    return nullptr;
  }

  bool leader_valid = false;
  if (ed->captions_style_leader != nullptr) {
    for (CaptionsStripRef &ref : ed->captions_strips) {
      if ((&ref == ed->captions_style_leader && ref.strip != nullptr && ed->captions_style_leader->use_custom_style == false)) {
        leader_valid = true;
        break;
      }
    }
  }
  
  if (!leader_valid) {
    /* Search for new leader */
    ed->captions_style_leader = nullptr;
    for (CaptionsStripRef &ref : ed->captions_strips) {
      if (ref.strip != nullptr) {
        ed->captions_style_leader = &ref;
        break;
      }
    }
  }
  
  return ed->captions_style_leader;
}

Strip *style_leader_strip_ensure(Editing *ed) {
  CaptionsStripRef *ref = style_leader_ref_ensure(ed);
  if(ref == nullptr /*|| ref->strip == nullptr*/) {
    return nullptr;
  }
  return ref->strip;
}

CaptionsStripRef *get_ref_by_strip(Editing *ed, struct Strip *strip) {
  for (CaptionsStripRef &ref : ed->captions_strips) {
    if (ref.strip == strip) {
      return &ref;
    }
  }
  return nullptr;
}

void mark_ref_style_custom(CaptionsStripRef *ref, bool use_custom) {
  if(ref != nullptr) {
    ref->use_custom_style = use_custom ? 1 : 0;
  }
}

static void update_strips_style(Scene *scene)
{
  if(scene == nullptr){
    return;
  }

  Editing *ed = seq::editing_get(scene);
  if(ed == nullptr){
    return;
  }
  
  Strip *leader_strip = style_leader_strip_ensure(ed);
  if(leader_strip == nullptr) {
    return;
  }

  TextVars *leader_vers = (TextVars *)leader_strip->effectdata;
  if(leader_vers == nullptr) {
    return;
  }

  for (CaptionsStripRef &ref : ed->captions_strips) {
    Strip *strip = ref.strip;
    TextVars *vers = (TextVars *)strip->effectdata;
    if(vers == nullptr) {
      continue;
    }

    if(vers != leader_vers) {
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
}
  
static void update_active_channel(Editing *ed){
  if (ed != nullptr) {
    ed->captions_act_channel = blender::seq::channel_get_by_index(&ed->channels, 1);
  }
}

static ListBaseT<struct CaptionsStripRef> build_strip_refs(Editing *ed)
{
  ListBaseT<CaptionsStripRef> result = {nullptr, nullptr};
  if(ed == nullptr){
    return result;
  }

  for (Strip &strip : ed->seqbase) {
    if (strip.channel == ed->captions_act_channel->index) {
      if (strip.type == STRIP_TYPE_TEXT) {
        CaptionsStripRef *ref = (CaptionsStripRef *)MEM_callocN(sizeof(CaptionsStripRef), "strip ref");
        ref->strip = &strip;
        BLI_addtail(&result, ref);
      }
    }
  }
  
  return result;
}

static void free_strip_refs(Editing *ed)
{
  if(ed == nullptr){
    return;
  }

  ListBase *refs = &ed->captions_strips;
  for (CaptionsStripRef &ref : ed->captions_strips.items_mutable()) {
    MEM_freeN(&ref);
  }
  BLI_listbase_clear(refs);

  ed->captions_style_leader = nullptr;
}

/* Can be used without scene just for redraw without update */
void tag_redraw(ARegion *region, Scene *scene)
{
  if(scene != nullptr) {
    update_current_strips(scene);
  }

  ED_region_tag_redraw(region);
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

void update_current_strips(Scene *scene) 
{
  Editing *ed = blender::seq::editing_get(scene);
  
  if (ed == nullptr) {
    return;
  }
  
  if (ed->captions_act_channel == nullptr) {
    update_active_channel(ed);
  }
   
  if (ed->captions_act_channel != nullptr) {
    
    /* Free old references */
    free_strip_refs(ed);

    ed->captions_strips = build_strip_refs(ed);
    
    ed->captions_cache_dirty =  false;

    BLI_listbase_sort(&ed->captions_strips, compare_strips_start);

    update_strips_style(scene);
  }
}

static SpaceLink *captions_create(const ScrArea * /*area*/, const Scene * scene)
{
    SpaceCaptions *scaptions = MEM_new_for_free<SpaceCaptions>("initcaptions");
   // scaptions->runtime = MEM_new<SpaceCaptions_Runtime>(__func__);
    scaptions->spacetype = SPACE_CAPTIONS;
    ARegion *region;
    
    /* header */
    region = BKE_area_region_new();
    BLI_addtail(&scaptions->regionbase, region);
    region->regiontype = RGN_TYPE_HEADER;
    region->alignment = (U.uiflag & USER_HEADER_BOTTOM) ? RGN_ALIGN_BOTTOM : RGN_ALIGN_TOP;

    /* main window */
    region = BKE_area_region_new();
    BLI_addtail(&scaptions->regionbase, region);
    region->regiontype = RGN_TYPE_WINDOW;

    update_current_strips((Scene *) scene);

    return (SpaceLink *)scaptions;
}

/* Doesn't free the space-link itself. */
static void captions_free(SpaceLink *sl)
{
  //free_strip_refs(scaptions);
}

/* spacetype; init callback, add handlers */
static void captions_init(wmWindowManager * /*wm*/, ScrArea * /*area*/) {}

static SpaceLink *captions_duplicate(SpaceLink *sl)
{
  SpaceCaptions *scaptionsn = static_cast<SpaceCaptions *>(MEM_dupallocN(sl));

  /* clear or remove stuff from old */

  return (SpaceLink *)scaptionsn;
}

static void captions_keymap(wmKeyConfig * /*keyconf*/)
{
 // WM_keymap_ensure(keyconf, "Image Generic", SPACE_IMAGE, RGN_TYPE_WINDOW);
  //WM_keymap_ensure(keyconf, "Image", SPACE_IMAGE, RGN_TYPE_WINDOW);
}

static void captions_refresh(const bContext * /*C*/, ScrArea * /*area*/)
{
}

static void captions_listener(const wmSpaceTypeListenerParams * /*params*/)
{
/*  wmWindow *win = params->window;
  ScrArea *area = params->area;
  const wmNotifier *wmn = params->notifier;
  SpaceCaptions *sima = (SpaceCaptions *)area->spacedata.first;*/
}

static int /*eContextResult*/ captions_context(const bContext *C,
                                            const char *member,
                                            bContextDataResult *result)
{
  Scene *scene = CTX_data_scene(C);
  
  if (CTX_data_dir(member)) {
    return CTX_RESULT_OK;
  }
  else if (CTX_data_equals(member, "scene")) {
    CTX_data_id_pointer_set(result, &scene->id);
    return CTX_RESULT_OK;
  }
  
  return CTX_RESULT_MEMBER_NOT_FOUND;
}

/************************** main region ***************************/

/* add handlers, stuff you only do once or on area/region changes */
static void captions_main_region_init(wmWindowManager *wm, ARegion *region)
{
region->v2d.scroll = V2D_SCROLL_RIGHT | V2D_SCROLL_VERTICAL_HIDE;

  ED_region_panels_init(wm, region);
}

static void captions_main_region_layout(const bContext *C, ARegion *region)
{
  //update_current_strips(C);
  //View2D *v2d = &region->v2d;

  //UI_view2d_region_reinit(&region->v2d, V2D_COMMONVIEW_PANELS_UI, region->winx, region->winy);
  ED_region_panels_layout(C, region);
}

static void captions_main_region_draw(const bContext *C, ARegion *region)
{

  ED_region_panels_draw(C, region);
}

/* TODO: Clean this method and make it more readable */
static void captions_main_region_listener(const wmRegionListenerParams *params)
{
  ScrArea *area = params->area;
  ARegion *region = params->region;
  const wmNotifier *wmn = params->notifier;
  /* context changes */
    switch (wmn->category) {
    case NC_SCENE:
      switch (wmn->data) {
        case ND_SEQUENCER:
          //printf("action: %d data: %d category: %d\n", wmn->action, wmn->data, wmn->category);
          switch (wmn->action) {
            case NA_ADDED:
            case NA_REMOVED:
            case NA_EDITED: {
                Scene *scene =  (Scene *)wmn->reference;
                Editing *ed = seq::editing_get(scene);
                if(ed == nullptr){
                  break;
                }

                  Strip *active_strip = blender::seq::select_active_get(scene);
                  if(active_strip != nullptr) { 
                    if(ed->captions_act_channel == nullptr){
                        update_active_channel(ed);
                    }
                    if(active_strip->channel == ed->captions_act_channel->index) {
                      if(active_strip->type == STRIP_TYPE_TEXT) {
                        ed->captions_cache_dirty = true;
                      }
                      if(wmn->action == NA_ADDED) {
                        update_strips_style(scene);
                      }
                    } else {
                      /* If edited, it means a strip could move out of active_channel and has to update */
                      if(wmn->action == NA_EDITED) {
                        ed->captions_cache_dirty = true;
                      }
                    }
                  } else {
                    if(wmn->action == NA_REMOVED) {
                      style_leader_ref_ensure(ed);
                      ed->captions_cache_dirty = true;
                    }
                  }
              tag_redraw(region, scene);
              break;
            }
            default: {
              tag_redraw(region, nullptr);
              break;
            }
          }
          break;
      }
      break;
    case NC_SPACE:
      if (wmn->data == ND_SPACE_CAPTIONS) {
        switch (wmn->action) {
          case NA_EDITED: {
              Scene *scene =  (Scene *)wmn->reference;
              if(scene != nullptr) {
                update_strips_style(scene);
              }
            break;
          }
          default:
            tag_redraw(region, nullptr);
            break;
        }
      }
      break;
    default:
      break;
  }
}

/************************* header region **************************/

/* add handlers, stuff you only do once or on area/region changes */
static void captions_header_region_init(wmWindowManager * /*wm*/, ARegion *region)
{
  ED_region_header_init(region);
}

static void captions_header_region_draw(const bContext *C, ARegion *region)
{
  //ScrArea *area = CTX_wm_area(C);
  //SpaceCaptions *sima = static_cast<SpaceCaptions *>(area->spacedata.first);

  ED_region_header(C, region);
}

static void captions_header_region_listener(const wmRegionListenerParams * /*params*/)
{
  //ARegion *region = params->region;
  //const wmNotifier *wmn = params->notifier;
}

static void captions_foreach_id(SpaceLink *space_link, LibraryForeachIDData *data)
{
    UNUSED_VARS(space_link, data);
}

static void captions_space_blend_read_data(BlendDataReader * /*reader*/, SpaceLink * /*sl*/)
{
}

static void captions_space_blend_write(BlendWriter *writer, SpaceLink *sl)
{
  writer->write_struct_cast<SpaceSeq>(sl);
}

/**************************** spacetype *****************************/

void ED_spacetype_captions()
{
  using namespace blender::ed;
  std::unique_ptr<SpaceType> st = std::make_unique<SpaceType>();
  ARegionType *art;

  st->spaceid = SPACE_CAPTIONS;
  STRNCPY_UTF8(st->name, "Captions");

  st->create = captions_create;
  st->free = captions_free;
  st->init = captions_init;
  st->duplicate = captions_duplicate;
  st->operatortypes = captions_operatortypes;
  //st->refresh = captions_refresh;
  st->listener = captions_listener;
  st->context = captions_context;
  st->foreach_id = captions_foreach_id;
  st->blend_read_data = captions_space_blend_read_data;
  st->blend_write = captions_space_blend_write;

  /* regions: main window */
  art = MEM_callocN<ARegionType>("spacetype captions region");
  art->regionid = RGN_TYPE_WINDOW;
  art->prefsizex = UI_SIDEBAR_PANEL_WIDTH;
  art->keymapflag = ED_KEYMAP_UI | ED_KEYMAP_VIEW2D;
  art->init = captions_main_region_init;
  art->layout = captions_main_region_layout;
  art->draw = captions_main_region_draw;
  art->listener = captions_main_region_listener;
  //art->lock = REGION_DRAW_LOCK_BAKING;
  BLI_addhead(&st->regiontypes, art);

  /* regions: header */
  art = MEM_callocN<ARegionType>("spacetype captions region");
  art->regionid = RGN_TYPE_HEADER;
  art->prefsizey = HEADERY;
  art->listener = captions_header_region_listener;
  art->init = captions_header_region_init;
  art->draw = captions_header_region_draw;

  BLI_addhead(&st->regiontypes, art);

  BKE_spacetype_register(std::move(st));
}

}  // namespace blender

