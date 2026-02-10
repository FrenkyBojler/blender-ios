/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Eyedropper (RGB Color)
 *
 * Defines:
 * - #UI_OT_eyedropper_color
 */

#include "MEM_guardedalloc.h"

#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BLI_math_vector.h"

#include "BKE_context.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "IMB_colormanagement.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_intern.hh"

#include "ED_clip.hh"
#include "ED_image.hh"
#include "ED_node.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "eyedropper_intern.hh"

namespace blender::ui {

struct Eyedropper {
  const ColorManagedDisplay *display = nullptr;

  PointerRNA ptr = {};
  PropertyRNA *prop = nullptr;
  int index = 0;
  bool is_undo = false;

  bool is_set = false;
  float init_col[3] = {}; /* for resetting on cancel */

  bool accum_start = false; /* has mouse been pressed */
  float accum_col[3] = {};
  int accum_tot = 0;

  ViewportColorSampleSession *viewport_session = nullptr;
};

static bool eyedropper_init(bContext *C, wmOperator *op)
{
  Eyedropper *eye = MEM_new<Eyedropper>(__func__);

  PropertyRNA *prop = RNA_struct_find_property(op->ptr, "prop_data_path");
  if (prop && RNA_property_is_set(op->ptr, prop)) {
    std::string prop_data_path = RNA_string_get(op->ptr, "prop_data_path");
    if (prop_data_path.empty()) {
      MEM_delete(eye);
      return false;
    }
    PointerRNA ctx_ptr = RNA_pointer_create_discrete(nullptr, RNA_Context, C);
    if (!RNA_path_resolve(&ctx_ptr, prop_data_path.c_str(), &eye->ptr, &eye->prop)) {
      BKE_reportf(op->reports, RPT_ERROR, "Could not resolve path '%s'", prop_data_path.c_str());
      MEM_delete(eye);
      return false;
    }
    eye->is_undo = true;
  }
  else {
    Button *but = context_active_but_prop_get(C, &eye->ptr, &eye->prop, &eye->index);
    if (but != nullptr) {
      eye->is_undo = button_flag_is_set(but, BUT_UNDO);
    }
  }

  const enum PropertySubType prop_subtype = eye->prop ? RNA_property_subtype(eye->prop) :
                                                        PropertySubType(0);

  if ((eye->ptr.data == nullptr) || (eye->prop == nullptr) ||
      (RNA_property_editable(&eye->ptr, eye->prop) == false) ||
      (RNA_property_array_length(&eye->ptr, eye->prop) < 3) ||
      (RNA_property_type(eye->prop) != PROP_FLOAT) ||
      (ELEM(prop_subtype, PROP_COLOR, PROP_COLOR_GAMMA) == 0))
  {
    MEM_delete(eye);
    return false;
  }
  op->customdata = eye;

  float col[4];
  RNA_property_float_get_array_at_most(&eye->ptr, eye->prop, col, ARRAY_SIZE(col));

  if (prop_subtype != PROP_COLOR) {
    Scene *scene = CTX_data_scene(C);
    const char *display_device;

    display_device = scene->display_settings.display_device;
    eye->display = IMB_colormanagement_display_get_named(display_device);

    /* store initial color */
    if (eye->display) {
      IMB_colormanagement_display_to_scene_linear_v3(col, eye->display);
    }
  }
  copy_v3_v3(eye->init_col, col);

  return true;
}

static void eyedropper_exit(bContext *C, wmOperator *op)
{
  Eyedropper *eye = static_cast<Eyedropper *>(op->customdata);
  wmWindow *window = CTX_wm_window(C);
  WM_cursor_modal_restore(window);

  ED_workspace_status_text(C, nullptr);

  if (eye->viewport_session) {
    MEM_delete(eye->viewport_session);
    eye->viewport_session = nullptr;
  }

  op->customdata = nullptr;
  MEM_delete(eye);
}

/* *** eyedropper_color_ helper functions *** */

bool eyedropper_color_sample_fl(bContext *C,
                                Eyedropper *eye,
                                const int event_xy[2],
                                float r_col[3])
{
  ScrArea *area = nullptr;

  int event_xy_win[2];
  wmWindow *win = WM_window_find_under_cursor(CTX_wm_window(C), event_xy, event_xy_win);
  if (win) {
    bScreen *screen = WM_window_get_active_screen(win);
    area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event_xy_win);
  }

  if (area) {
    ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event_xy_win);
    if (region) {
      const int mval[2] = {
          event_xy_win[0] - region->winrct.xmin,
          event_xy_win[1] - region->winrct.ymin,
      };
      if (area->spacetype == SPACE_IMAGE) {
        SpaceImage *sima = static_cast<SpaceImage *>(area->spacedata.first);
        if (ED_space_image_color_sample(sima, region, mval, r_col, nullptr)) {
          return true;
        }
      }
      else if (area->spacetype == SPACE_NODE) {
        SpaceNode *snode = static_cast<SpaceNode *>(area->spacedata.first);
        Main *bmain = CTX_data_main(C);
        if (ED_space_node_color_sample(bmain, snode, region, mval, r_col)) {
          return true;
        }
      }
      else if (area->spacetype == SPACE_CLIP) {
        SpaceClip *sc = static_cast<SpaceClip *>(area->spacedata.first);
        if (ED_space_clip_color_sample(sc, region, mval, r_col)) {
          return true;
        }
      }
      else if (eye != nullptr && area->spacetype == SPACE_VIEW3D) {
        /* Viewport color picking involves a fairly expensive operation to copy the GPU viewport
         * back to the CPU, so to support smooth dragging with the eyedropper, we keep the copy
         * around for the entire operation. */
        if (eye->viewport_session == nullptr) {
          eye->viewport_session = MEM_new<ViewportColorSampleSession>("viewport_session");
          eye->viewport_session->init(region);
        }
        if (eye->viewport_session->sample(mval, r_col)) {
          return true;
        }
      }
    }
  }

  /* Other areas within a Blender window. */
  if (win) {
    if (!WM_window_pixels_read_sample(C, win, event_xy_win, r_col)) {
      WM_window_pixels_read_sample_from_offscreen(C, win, event_xy_win, r_col);
    }
    const char *display_device = CTX_data_scene(C)->display_settings.display_device;
    const ColorManagedDisplay *display = IMB_colormanagement_display_get_named(display_device);
    IMB_colormanagement_display_to_scene_linear_v3(r_col, display);
    return true;
  }

  /* Outside the Blender window if we support it. */
  if (WM_capabilities_flag() & WM_CAPABILITY_DESKTOP_SAMPLE) {
    if (WM_desktop_cursor_sample_read(r_col)) {
      IMB_colormanagement_srgb_to_scene_linear_v3(r_col, r_col);
      return true;
    }
  }

  zero_v3(r_col);
  return false;
}

/* sets the sample color RGB, maintaining A */
static void eyedropper_color_set(bContext *C, Eyedropper *eye, const float col[3])
{
  float col_conv[4];

  /* to maintain alpha */
  RNA_property_float_get_array_at_most(&eye->ptr, eye->prop, col_conv, ARRAY_SIZE(col_conv));

  /* convert from linear rgb space to display space */
  if (eye->display) {
    copy_v3_v3(col_conv, col);
    IMB_colormanagement_scene_linear_to_display_v3(col_conv, eye->display);
  }
  else {
    copy_v3_v3(col_conv, col);
  }

  RNA_property_float_set_array_at_most(&eye->ptr, eye->prop, col_conv, ARRAY_SIZE(col_conv));
  eye->is_set = true;

  RNA_property_update(C, &eye->ptr, eye->prop);
}

static void eyedropper_color_sample(bContext *C, Eyedropper *eye, const int event_xy[2])
{
  /* Accumulate color. */
  float col[3];
  if (!eyedropper_color_sample_fl(C, eye, event_xy, col)) {
    return;
  }

  add_v3_v3(eye->accum_col, col);
  eye->accum_tot++;

  /* Apply to property. */
  float accum_col[3];
  if (eye->accum_tot > 1) {
    mul_v3_v3fl(accum_col, eye->accum_col, 1.0f / float(eye->accum_tot));
  }
  else {
    copy_v3_v3(accum_col, eye->accum_col);
  }
  eyedropper_color_set(C, eye, accum_col);
}

static void eyedropper_cancel(bContext *C, wmOperator *op)
{
  Eyedropper *eye = static_cast<Eyedropper *>(op->customdata);
  if (eye->is_set) {
    eyedropper_color_set(C, eye, eye->init_col);
  }
  eyedropper_exit(C, op);
}

/* main modal status check */
static wmOperatorStatus eyedropper_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  Eyedropper *eye = static_cast<Eyedropper *>(op->customdata);

  /* handle modal keymap */
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case EYE_MODAL_CANCEL:
        eyedropper_cancel(C, op);
        return OPERATOR_CANCELLED;
      case EYE_MODAL_SAMPLE_CONFIRM: {
        const bool is_undo = eye->is_undo;
        if (eye->accum_tot == 0) {
          eyedropper_color_sample(C, eye, event->xy);
        }
        eyedropper_exit(C, op);
        /* Could support finished & undo-skip. */
        return is_undo ? OPERATOR_FINISHED : OPERATOR_CANCELLED;
      }
      case EYE_MODAL_SAMPLE_BEGIN:
        /* enable accum and make first sample */
        eye->accum_start = true;
        eyedropper_color_sample(C, eye, event->xy);
        break;
      case EYE_MODAL_SAMPLE_RESET:
        eye->accum_tot = 0;
        zero_v3(eye->accum_col);
        eyedropper_color_sample(C, eye, event->xy);
        break;
    }
  }
  else if (ISMOUSE_MOTION(event->type)) {
    if (eye->accum_start) {
      /* button is pressed so keep sampling */
      eyedropper_color_sample(C, eye, event->xy);
      WorkspaceStatus status(C);
      status.item(TIP_("Drag to continue sampling, release when done"), ICON_MOUSE_MOVE);
    }
    else {
      WorkspaceStatus status(C);
      status.opmodal(IFACE_("Confirm"), op->type, EYE_MODAL_SAMPLE_CONFIRM);
      status.opmodal(IFACE_("Cancel"), op->type, EYE_MODAL_CANCEL);
#ifdef __APPLE__
      status.item(TIP_("Press 'Enter' to sample outside of a Blender window"), ICON_INFO);
#endif
    }
  }

  return OPERATOR_RUNNING_MODAL;
}

/* Modal Operator init */
static wmOperatorStatus eyedropper_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  /* init */
  if (eyedropper_init(C, op)) {
    wmWindow *win = CTX_wm_window(C);
    /* Workaround for de-activating the button clearing the cursor, see #76794 */
    context_active_but_clear(C, win, CTX_wm_region(C));
    WM_cursor_modal_set(win, WM_CURSOR_EYEDROPPER);

    /* add temp handler */
    WM_event_add_modal_handler(C, op);

    return OPERATOR_RUNNING_MODAL;
  }
  return OPERATOR_PASS_THROUGH;
}

/* Repeat operator */
static wmOperatorStatus eyedropper_exec(bContext *C, wmOperator *op)
{
  /* init */
  if (eyedropper_init(C, op)) {

    /* do something */

    /* cleanup */
    eyedropper_exit(C, op);

    return OPERATOR_FINISHED;
  }
  return OPERATOR_PASS_THROUGH;
}

static bool eyedropper_poll(bContext *C)
{
  /* Actual test for active button happens later, since we don't
   * know which one is active until mouse over. */
  return (CTX_wm_window(C) != nullptr);
}

void UI_OT_eyedropper_color(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Eyedropper";
  ot->idname = "UI_OT_eyedropper_color";
  ot->description = "Sample a color from the Blender window to store in a property";

  /* API callbacks. */
  ot->invoke = eyedropper_invoke;
  ot->modal = eyedropper_modal;
  ot->cancel = eyedropper_cancel;
  ot->exec = eyedropper_exec;
  ot->poll = eyedropper_poll;

  /* flags */
  ot->flag = OPTYPE_UNDO | OPTYPE_BLOCKING | OPTYPE_INTERNAL;

  /* Paths relative to the context. */
  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna,
                        "prop_data_path",
                        nullptr,
                        0,
                        "Data Path",
                        "Path of property to be set with the depth");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

}  // namespace blender::ui
