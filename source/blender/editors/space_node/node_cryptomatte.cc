/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 * \brief Cryptomatte V2 picking operators.
 */

#include "MEM_guardedalloc.h"

#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_enums.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_listbase.h"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_string_ref.hh"

#include "BKE_context.hh"
#include "BKE_cryptomatte.h"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_main_invariants.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_screen.hh"

#include "NOD_composite.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_clip.hh"
#include "ED_image.hh"
#include "ED_node.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "RE_pipeline.h"

#include "UI_interface_c.hh"
#include "UI_resources.hh"

#include "node_intern.hh"

namespace blender::ed::space_node {

/* Must match Eyedropper Modal Map values from interface_eyedropper.cc. */
enum {
  EYE_MODAL_CANCEL = 1,
  EYE_MODAL_SAMPLE_CONFIRM,
  EYE_MODAL_SAMPLE_BEGIN,
  EYE_MODAL_SAMPLE_RESET,
};

struct CryptomattePicker {
  bNode *node = nullptr;
  bNodeTree *ntree = nullptr;
  CryptomatteSession *session = nullptr;
  bool is_add = true;
  wmWindow *cb_win = nullptr;
  int cb_win_event_xy[2] = {};
  void *draw_handle_sample_text = nullptr;
  char sample_text[MAX_NAME] = {};
};

/* -------------------------------------------------------------------- */
/** \name Draw Callback
 * \{ */

static void cryptomatte_draw_cb(const wmWindow * /*window*/, void *arg)
{
  CryptomattePicker *picker = static_cast<CryptomattePicker *>(arg);
  if (picker->sample_text[0] == '\0') {
    return;
  }

  const uiFontStyle *fstyle = UI_FSTYLE_WIDGET;
  const bTheme *btheme = ui::theme::theme_get();
  const uiWidgetColors *wcol = &btheme->tui.wcol_tooltip;

  float col_fg[4], col_bg[4];
  rgba_uchar_to_float(col_fg, wcol->text);
  rgba_uchar_to_float(col_bg, wcol->inner);

  ui::fontstyle_draw_simple_backdrop(
      fstyle, picker->cb_win_event_xy[0], picker->cb_win_event_xy[1] + U.widget_unit,
      picker->sample_text, col_fg, col_bg);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sampling Functions
 * \{ */

static bool cryptomatte_sample_view3d_fl(bContext *C,
                                          const char *prefix,
                                          const int mval[2],
                                          float r_col[3])
{
  int material_slot = 0;
  Object *object = ED_view3d_give_material_slot_under_cursor(C, mval, &material_slot);
  if (!object) {
    return false;
  }

  const ID *id = nullptr;
  if (StringRef(prefix).endswith(RE_PASSNAME_CRYPTOMATTE_OBJECT)) {
    id = &object->id;
  }
  else if (StringRef(prefix).endswith(RE_PASSNAME_CRYPTOMATTE_MATERIAL)) {
    Material *material = BKE_object_material_get(object, material_slot);
    if (!material) {
      return false;
    }
    id = &material->id;
  }

  if (!id) {
    return false;
  }

  const char *name = &id->name[2];
  const int name_length = BLI_strnlen(name, MAX_NAME - 2);
  uint32_t cryptomatte_hash = BKE_cryptomatte_hash(name, name_length);
  r_col[0] = BKE_cryptomatte_hash_to_float(cryptomatte_hash);
  return true;
}

static bool cryptomatte_sample_renderlayer_fl(RenderLayer *render_layer,
                                               const char *prefix,
                                               const float fpos[2],
                                               float r_col[3])
{
  if (!render_layer) {
    return false;
  }

  const int render_layer_name_len = STRNLEN(render_layer->name);
  if (strncmp(prefix, render_layer->name, render_layer_name_len) != 0) {
    return false;
  }

  const int prefix_len = strlen(prefix);
  if (prefix_len <= render_layer_name_len + 1) {
    return false;
  }

  /* RenderResult from images can have no render layer name. */
  const char *render_pass_name_prefix = render_layer_name_len ?
                                            prefix + 1 + render_layer_name_len :
                                            prefix;

  for (RenderPass &render_pass : render_layer->passes) {
    if (STRPREFIX(render_pass.name, render_pass_name_prefix) &&
        !STREQLEN(render_pass.name, render_pass_name_prefix, sizeof(render_pass.name)))
    {
      BLI_assert(render_pass.channels == 4);

      if (!render_pass.ibuf) {
        return false;
      }

      const int x = int(fpos[0] * render_pass.rectx);
      const int y = int(fpos[1] * render_pass.recty);
      const int offset = 4 * (y * render_pass.rectx + x);
      zero_v3(r_col);
      r_col[0] = render_pass.ibuf->float_buffer.data[offset];
      return true;
    }
  }

  return false;
}

static bool cryptomatte_sample_render_fl(const bNode *node,
                                          const char *prefix,
                                          const float fpos[2],
                                          float r_col[3])
{
  bool success = false;
  Scene *scene = id_cast<Scene *>(node->id);
  BLI_assert(GS(scene->id.name) == ID_SCE);
  Render *re = RE_GetSceneRender(scene);

  if (re) {
    RenderResult *rr = RE_AcquireResultRead(re);
    if (rr) {
      for (ViewLayer &view_layer : scene->view_layers) {
        RenderLayer *render_layer = RE_GetRenderLayer(rr, view_layer.name);
        success = cryptomatte_sample_renderlayer_fl(render_layer, prefix, fpos, r_col);
        if (success) {
          break;
        }
      }
    }
    RE_ReleaseResult(re);
  }
  return success;
}

static bool cryptomatte_sample_image_fl(bContext *C,
                                         const bNode *node,
                                         NodeCryptomatte *crypto,
                                         const char *prefix,
                                         const float fpos[2],
                                         float r_col[3])
{
  bool success = false;
  Image *image = id_cast<Image *>(node->id);
  BLI_assert((image == nullptr) || (GS(image->id.name) == ID_IM));

  Scene *scene = CTX_data_scene(C);
  ImageUser image_user_for_frame = crypto->iuser;
  BKE_image_user_frame_calc(image, &image_user_for_frame, scene->r.cfra);

  if (image && image->type == IMA_TYPE_MULTILAYER) {
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, &image_user_for_frame, nullptr);
    if (image->rr) {
      for (RenderLayer &render_layer : image->rr->layers) {
        success = cryptomatte_sample_renderlayer_fl(&render_layer, prefix, fpos, r_col);
        if (success) {
          break;
        }
      }
    }
    BKE_image_release_ibuf(image, ibuf, nullptr);
  }
  return success;
}

static bool cryptomatte_sample_fl(bContext *C,
                                   CryptomattePicker *picker,
                                   const int event_xy[2],
                                   float r_col[3])
{
  bNode *node = picker->node;
  NodeCryptomatte *crypto = node ? (static_cast<NodeCryptomatte *>(node->storage)) : nullptr;

  if (!crypto) {
    return false;
  }

  ScrArea *area = nullptr;

  int event_xy_win[2];
  wmWindow *win = WM_window_find_under_cursor(CTX_wm_window(C), event_xy, event_xy_win);
  if (win) {
    bScreen *screen = WM_window_get_active_screen(win);
    area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event_xy_win);
  }

  picker->cb_win_event_xy[0] = event_xy_win[0];
  picker->cb_win_event_xy[1] = event_xy_win[1];

  if (win && win != picker->cb_win && picker->draw_handle_sample_text) {
    WM_draw_cb_exit(picker->cb_win, picker->draw_handle_sample_text);
    picker->cb_win = win;
    picker->draw_handle_sample_text = WM_draw_cb_activate(
        picker->cb_win, cryptomatte_draw_cb, picker);
    ED_region_tag_redraw(CTX_wm_region(C));
  }

  if (!area || !ELEM(area->spacetype, SPACE_IMAGE, SPACE_NODE, SPACE_CLIP, SPACE_VIEW3D)) {
    return false;
  }

  ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event_xy_win);

  if (!region) {
    return false;
  }

  const int mval[2] = {
      event_xy_win[0] - region->winrct.xmin,
      event_xy_win[1] - region->winrct.ymin,
  };
  float fpos[2] = {-1.0f, -1.0f};
  switch (area->spacetype) {
    case SPACE_IMAGE: {
      SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
      ED_space_image_get_position(sima, region, mval, fpos);
      break;
    }
    case SPACE_NODE: {
      Main *bmain = CTX_data_main(C);
      SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
      ED_space_node_get_position(bmain, snode, region, mval, fpos);
      break;
    }
    case SPACE_CLIP: {
      SpaceClip *sc = static_cast<SpaceClip *>(area->spacedata.first);
      ED_space_clip_get_position(sc, region, mval, fpos);
      break;
    }
    default: {
      break;
    }
  }

  if (area->spacetype != SPACE_VIEW3D &&
      (fpos[0] < 0.0f || fpos[1] < 0.0f || fpos[0] >= 1.0f || fpos[1] >= 1.0f))
  {
    return false;
  }

  /* CMP_NODE_CRYPTOMATTE_SOURCE_RENDER and CMP_NODE_CRYPTOMATTE_SOURCE_IMAGE require a referenced
   * image/scene to work properly. */
  if (!node->id) {
    return false;
  }

  ED_region_tag_redraw(region);

  char prefix[MAX_NAME + 1];
  ntreeCompositCryptomatteLayerPrefix(node, prefix, sizeof(prefix) - 1);
  prefix[MAX_NAME] = '\0';

  if (area->spacetype == SPACE_VIEW3D) {
    wmWindow *win_prev = CTX_wm_window(C);
    ScrArea *area_prev = CTX_wm_area(C);
    ARegion *region_prev = CTX_wm_region(C);

    CTX_wm_window_set(C, win);
    CTX_wm_area_set(C, area);
    CTX_wm_region_set(C, region);

    const bool success = cryptomatte_sample_view3d_fl(C, prefix, mval, r_col);

    CTX_wm_window_set(C, win_prev);
    CTX_wm_area_set(C, area_prev);
    CTX_wm_region_set(C, region_prev);

    return success;
  }
  if (node->custom1 == CMP_NODE_CRYPTOMATTE_SOURCE_RENDER) {
    return cryptomatte_sample_render_fl(node, prefix, fpos, r_col);
  }
  if (node->custom1 == CMP_NODE_CRYPTOMATTE_SOURCE_IMAGE) {
    return cryptomatte_sample_image_fl(C, node, crypto, prefix, fpos, r_col);
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator Callbacks
 * \{ */

static void cryptomatte_pick_sample_text_update(bContext *C,
                                                 CryptomattePicker *picker,
                                                 const int event_xy[2])
{
  float col[3];
  picker->sample_text[0] = '\0';

  if (picker->session) {
    if (cryptomatte_sample_fl(C, picker, event_xy, col)) {
      BKE_cryptomatte_find_name(
          picker->session, col[0], picker->sample_text, sizeof(picker->sample_text));
      picker->sample_text[sizeof(picker->sample_text) - 1] = '\0';
    }
  }
}

static void cryptomatte_pick_exit(bContext *C, wmOperator *op)
{
  CryptomattePicker *picker = static_cast<CryptomattePicker *>(op->customdata);
  wmWindow *window = CTX_wm_window(C);
  WM_cursor_modal_restore(window);

  if (picker->draw_handle_sample_text) {
    WM_draw_cb_exit(picker->cb_win, picker->draw_handle_sample_text);
    picker->draw_handle_sample_text = nullptr;
  }

  if (picker->session) {
    BKE_cryptomatte_free(picker->session);
    picker->session = nullptr;
  }

  op->customdata = nullptr;
  MEM_delete(picker);
}

static void cryptomatte_pick_cancel(bContext *C, wmOperator *op)
{
  cryptomatte_pick_exit(C, op);
}

static bool cryptomatte_pick_poll(bContext *C)
{
  if (!ED_operator_node_editable(C)) {
    return false;
  }
  SpaceNode *snode = CTX_wm_space_node(C);
  return ED_node_is_compositor(snode);
}

static wmOperatorStatus cryptomatte_pick_invoke(bContext *C,
                                                 wmOperator *op,
                                                 const wmEvent * /*event*/)
{
  PointerRNA ptr = CTX_data_pointer_get(C, "node");
  bNode *node = nullptr;
  bNodeTree *ntree = nullptr;

  if (ptr.data) {
    node = static_cast<bNode *>(ptr.data);
    ntree = id_cast<bNodeTree *>(ptr.owner_id);
  }
  else {
    SpaceNode *snode = CTX_wm_space_node(C);
    if (snode && snode->edittree) {
      ntree = snode->edittree;
      node = bke::node_get_active(*snode->edittree);
    }
  }

  if (!node || node->type_legacy != CMP_NODE_CRYPTOMATTE) {
    return OPERATOR_CANCELLED;
  }

  CryptomattePicker *picker = MEM_new<CryptomattePicker>(__func__);
  picker->node = node;
  picker->ntree = ntree;
  picker->session = ntreeCompositCryptomatteSession(node);
  picker->is_add = RNA_boolean_get(op->ptr, "is_add");
  picker->cb_win = CTX_wm_window(C);
  picker->draw_handle_sample_text = WM_draw_cb_activate(
      picker->cb_win, cryptomatte_draw_cb, picker);

  op->customdata = picker;

  wmWindow *win = CTX_wm_window(C);
  WM_cursor_modal_set(win, WM_CURSOR_EYEDROPPER);

  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus cryptomatte_pick_modal(bContext *C,
                                                wmOperator *op,
                                                const wmEvent *event)
{
  CryptomattePicker *picker = static_cast<CryptomattePicker *>(op->customdata);

  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case EYE_MODAL_CANCEL:
        cryptomatte_pick_exit(C, op);
        return OPERATOR_CANCELLED;

      case EYE_MODAL_SAMPLE_CONFIRM: {
        float col[3];
        if (cryptomatte_sample_fl(C, picker, event->xy, col)) {
          bNode *node = picker->node;
          NodeCryptomatte *crypto = static_cast<NodeCryptomatte *>(node->storage);

          if (picker->is_add) {
            copy_v3_fl(crypto->runtime.add, col[0]);
            ntreeCompositCryptomatteSyncFromAdd(node);
          }
          else {
            copy_v3_fl(crypto->runtime.remove, col[0]);
            ntreeCompositCryptomatteSyncFromRemove(node);
          }

          BKE_ntree_update_tag_node_property(picker->ntree, node);
          BKE_main_ensure_invariants(*CTX_data_main(C), picker->ntree->id);
        }

        cryptomatte_pick_exit(C, op);
        return OPERATOR_FINISHED;
      }

      case EYE_MODAL_SAMPLE_BEGIN:
      case EYE_MODAL_SAMPLE_RESET:
        break;
    }
  }
  else if (ISMOUSE_MOTION(event->type)) {
    cryptomatte_pick_sample_text_update(C, picker, event->xy);
  }

  return OPERATOR_RUNNING_MODAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator Registration
 * \{ */

static void cryptomatte_entry_op_define(wmOperatorType *ot, bool is_add)
{
  ot->invoke = cryptomatte_pick_invoke;
  ot->modal = cryptomatte_pick_modal;
  ot->cancel = cryptomatte_pick_cancel;
  ot->poll = cryptomatte_pick_poll;

  ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING | OPTYPE_INTERNAL;

  PropertyRNA *prop = RNA_def_boolean(
      ot->srna, "is_add", is_add, "Is Add", "Whether to add or remove the entry");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

void NODE_OT_cryptomatte_entry_add(wmOperatorType *ot)
{
  ot->name = "Add Cryptomatte Entry";
  ot->idname = "NODE_OT_cryptomatte_entry_add";
  ot->description = "Add an entry to the matte by picking from the compositor result";

  cryptomatte_entry_op_define(ot, true);
}

void NODE_OT_cryptomatte_entry_remove(wmOperatorType *ot)
{
  ot->name = "Remove Cryptomatte Entry";
  ot->idname = "NODE_OT_cryptomatte_entry_remove";
  ot->description = "Remove an entry from the matte by picking from the compositor result";

  cryptomatte_entry_op_define(ot, false);
}

/** \} */

}  // namespace blender::ed::space_node
