/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "BKE_context.hh"

#include "BLI_math_base.h"

#include "WM_api.hh"

#include "ED_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "view3d_intern.hh"
#include "view3d_navigate.hh" /* own include */

namespace blender {

/* -------------------------------------------------------------------- */
/** \name View Scale Operator
 * \{ */

namespace {

constexpr float VIEW_SCALE_STRETCH_MIN = -0.95f;
constexpr float VIEW_SCALE_STRETCH_MAX = 8.0f;
constexpr float VIEW_SCALE_SENSITIVITY = 2.0f;

static const EnumPropertyItem viewscale_axis_items[] = {
    {VIEW_SCALE_AXIS_BOTH, "BOTH", 0, "Both", "Stretch horizontally and vertically"},
    {VIEW_SCALE_AXIS_HORIZONTAL, "HORIZONTAL", 0, "Horizontal", "Stretch horizontally only"},
    {VIEW_SCALE_AXIS_VERTICAL, "VERTICAL", 0, "Vertical", "Stretch vertically only"},
    {0, nullptr, 0, nullptr, nullptr},
};

static eViewScaleAxis viewscale_axis_from_event(const wmEvent *event, PointerRNA *ptr)
{
  eViewScaleAxis axis = eViewScaleAxis(RNA_enum_get(ptr, "axis"));

  if ((event->modifier & KM_SHIFT) && ((event->modifier & KM_CTRL) == 0)) {
    axis = VIEW_SCALE_AXIS_HORIZONTAL;
  }
  else if ((event->modifier & KM_CTRL) && ((event->modifier & KM_SHIFT) == 0)) {
    axis = VIEW_SCALE_AXIS_VERTICAL;
  }

  return axis;
}

}  // namespace

void viewscale_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {VIEW_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {VIEW_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},

      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "View3D Scale Modal");

  if (keymap && keymap->modal_items) {
    return;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "View3D Scale Modal", modal_items);

  WM_modalkeymap_assign(keymap, "VIEW3D_OT_view_scale");
}

static bool viewscale_poll(bContext *C)
{
  if (!view3d_zoom_or_dolly_poll(C)) {
    return false;
  }

  const RegionView3D *rv3d = CTX_wm_region_view3d(C);
  return rv3d != nullptr && rv3d->persp == RV3D_ORTHO;
}

static void viewscale_apply(ViewOpsData *vod, const int xy[2])
{
  const int xy_curr_offset[2] = {
      xy[0] + vod->init.event_xy_offset[0],
      xy[1] + vod->init.event_xy_offset[1],
  };
  const int xy_init_offset[2] = {
      vod->init.event_xy[0] + vod->init.event_xy_offset[0],
      vod->init.event_xy[1] + vod->init.event_xy_offset[1],
  };

  const float delta_x = float(xy_curr_offset[0] - xy_init_offset[0]) /
                        max_ff(float(vod->region->winx), 1.0f);
  const float delta_y = float(xy_curr_offset[1] - xy_init_offset[1]) /
                        max_ff(float(vod->region->winy), 1.0f);

  if (ELEM(vod->viewscale_axis, VIEW_SCALE_AXIS_BOTH, VIEW_SCALE_AXIS_HORIZONTAL)) {
    vod->rv3d->viewscale_x = clamp_f(
        vod->init.viewscale_x + (delta_x * VIEW_SCALE_SENSITIVITY),
        VIEW_SCALE_STRETCH_MIN,
        VIEW_SCALE_STRETCH_MAX);
  }

  if (ELEM(vod->viewscale_axis, VIEW_SCALE_AXIS_BOTH, VIEW_SCALE_AXIS_VERTICAL)) {
    vod->rv3d->viewscale_y = clamp_f(
        vod->init.viewscale_y + (delta_y * VIEW_SCALE_SENSITIVITY),
        VIEW_SCALE_STRETCH_MIN,
        VIEW_SCALE_STRETCH_MAX);
  }

  ED_region_tag_redraw(vod->region);
  ED_view3d_update_viewmat(
      vod->depsgraph, vod->scene, vod->v3d, vod->region, nullptr, nullptr, nullptr, false);
}

static wmOperatorStatus viewscale_modal_impl(bContext * /*C*/,
                                             ViewOpsData *vod,
                                             const eV3D_OpEvent event_code,
                                             const int xy[2])
{
  switch (event_code) {
    case VIEW_APPLY:
      viewscale_apply(vod, xy);
      return OPERATOR_RUNNING_MODAL;
    case VIEW_CONFIRM: {
      /* If the mouse never moved (a plain click), treat it as a toggle: reset all
       * stretch to zero. This lets the user click to turn stretch off. */
      const bool did_drag = (vod->rv3d->viewscale_x != vod->init.viewscale_x ||
                             vod->rv3d->viewscale_y != vod->init.viewscale_y);
      if (!did_drag && (vod->init.viewscale_x != 0.0f || vod->init.viewscale_y != 0.0f)) {
        vod->rv3d->viewscale_x = 0.0f;
        vod->rv3d->viewscale_y = 0.0f;
        ED_view3d_update_viewmat(
            vod->depsgraph, vod->scene, vod->v3d, vod->region, nullptr, nullptr, nullptr, false);
        ED_region_tag_redraw(vod->region);
      }
      return OPERATOR_FINISHED;
    }
    case VIEW_CANCEL:
      vod->state_restore();
      return OPERATOR_CANCELLED;
    case VIEW_PASS:
      return OPERATOR_RUNNING_MODAL;
  }

  BLI_assert_unreachable();
  return OPERATOR_CANCELLED;
}

static wmOperatorStatus viewscale_invoke_impl(bContext * /*C*/,
                                              ViewOpsData *vod,
                                              const wmEvent *event,
                                              PointerRNA *ptr)
{
  vod->viewscale_axis = viewscale_axis_from_event(event, ptr);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus viewscale_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  return view3d_navigate_invoke_impl(C, op, event, &ViewOpsType_scale);
}

void VIEW3D_OT_view_scale(wmOperatorType *ot)
{
  ot->name = "Scale View";
  ot->description = "Stretch the orthographic view horizontally and vertically";
  ot->idname = ViewOpsType_scale.idname;

  ot->invoke = viewscale_invoke;
  ot->modal = view3d_navigate_modal_fn;
  ot->poll = viewscale_poll;
  ot->cancel = view3d_navigate_cancel_fn;

  ot->flag = OPTYPE_BLOCKING | OPTYPE_GRAB_CURSOR_XY;

  view3d_operator_properties_common(ot, V3D_OP_PROP_USE_MOUSE_INIT);

  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "axis", viewscale_axis_items, VIEW_SCALE_AXIS_BOTH, "Axis", "Axes to stretch");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

const ViewOpsType ViewOpsType_scale = {
    /*flag*/ VIEWOPS_FLAG_NONE,
    /*idname*/ "VIEW3D_OT_view_scale",
    /*poll_fn*/ viewscale_poll,
    /*init_fn*/ viewscale_invoke_impl,
    /*apply_fn*/ viewscale_modal_impl,
};

/** \} */

}  // namespace blender
