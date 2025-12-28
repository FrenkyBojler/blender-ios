/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spcaptions
 */

#include "DNA_defaults.h"
#include "DNA_space_types.h"
#include "DNA_object_types.h"
#include "DNA_sequence_types.h"
#include "DNA_captions_types.h"

#include "captions_intern.hh"

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"
#include "BLI_threads.h"

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

#include "ED_screen.hh"
#include "BLO_read_write.hh"

static void update_active_channel(Scene *scene, SpaceCaptions *scaptions){
  Editing *ed = blender::seq::editing_get(scene);
  if (ed != nullptr) {
    scaptions->active_channel = blender::seq::channel_get_by_index(&ed->channels, 1);
  }
}

static ListBase build_strip_refs(ListBase *strips, int channel_index)
{
  ListBase result = {nullptr, nullptr};
  LISTBASE_FOREACH (Strip *, strip, strips) {
    if (strip->channel == channel_index) {
      if (strip->type == STRIP_TYPE_TEXT) {
        CaptionsStripRef *ref = (CaptionsStripRef *)MEM_callocN(sizeof(CaptionsStripRef), "strip ref");
        ref->strip = strip;
        BLI_addtail(&result, ref);
      }
    }
  }
  
  return result;
}

static void free_strip_refs(SpaceCaptions *scaptions)
{
  ListBase *refs = &scaptions->current_strips;
  LISTBASE_FOREACH_MUTABLE (CaptionsStripRef *, ref, refs) {
    MEM_freeN(ref);
  }
  BLI_listbase_clear(refs);
}

/* Can be used without scene and scaptions, just for redraw without update */
void tag_redraw(ARegion *region, Scene *scene, SpaceCaptions *scaptions)
{
  if(scene != nullptr && scaptions != nullptr){
    update_current_strips(scaptions->seq_scene, scaptions);
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

void update_current_strips(Scene *scene, SpaceCaptions *scaptions) 
{
  Editing *ed = blender::seq::editing_get(scene);
  
  if (ed == nullptr) {
    return;
  }
  
  if (scaptions->active_channel == nullptr) {
    update_active_channel(scene, scaptions);
  }
   
  if (scaptions->active_channel != nullptr) {
    // Free old references
    free_strip_refs(scaptions);
    
    // Build new reference list
    scaptions->current_strips = build_strip_refs(&ed->seqbase, scaptions->active_channel->index);
    
    scaptions->seq_scene = scene;
    scaptions->cache_dirty = false;

    BLI_listbase_sort(&scaptions->current_strips, compare_strips_start);
  }
}

static SpaceLink *captions_create(const ScrArea * /*area*/, const Scene * scene)
{
    SpaceCaptions *scaptions = MEM_callocN<SpaceCaptions>("initcaptions");
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

    update_current_strips((Scene *) scene, scaptions);

    return (SpaceLink *)scaptions;
}

/* Doesn't free the space-link itself. */
static void captions_free(SpaceLink *sl)
{
  SpaceCaptions *scaptions = (SpaceCaptions *)sl;

  free_strip_refs(scaptions);
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
          printf("action: %d data: %d category: %d\n", wmn->action, wmn->data, wmn->category);
          switch (wmn->action) {
            case NA_ADDED:
            case NA_REMOVED:
            case NA_EDITED: {
                SpaceCaptions *scaptions = (SpaceCaptions *)area->spacedata.first;
                if(scaptions == nullptr) {
                  break;
                }

                Scene *scene =  (Scene *)wmn->reference;
                if(scene == nullptr) {
                  if(scaptions->seq_scene != nullptr) {
                    scene = scaptions->seq_scene;
                  } else {
                    break;
                  }
                }

                  Strip *active_strip = blender::seq::select_active_get(scene);
                  if(active_strip != nullptr) { 
                    if(scaptions -> active_channel == nullptr){
                        update_active_channel(scene, scaptions);
                    }
                    if(active_strip->channel == scaptions->active_channel->index) {
                      if(active_strip->type == STRIP_TYPE_TEXT) {
                        scaptions->cache_dirty = true;
                      }
                    } else {
                      /* If edited, it means a strip could move out of active_channel and has to update */
                      if(wmn->action == NA_EDITED) {
                        scaptions->cache_dirty = true;
                      }
                    }
                  } else {
                    if(wmn->action == NA_REMOVED) {
                      scaptions->cache_dirty = true;
                    }
                  }
              tag_redraw(region, scene, scaptions);
              break;
            }
            default: {
              tag_redraw(region, nullptr, nullptr);
              break;
            }
          }
          break;
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
  BLO_write_struct(writer, SpaceCaptions, sl);
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
