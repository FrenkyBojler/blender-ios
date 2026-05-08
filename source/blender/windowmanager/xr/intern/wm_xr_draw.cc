/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * \name Window-Manager XR Drawing
 *
 * Implements Blender specific drawing functionality for use with the Ghost-XR API.
 */

#include <cstring>
#include <cfloat>

#include "DNA_userdef_types.h"
#include "DNA_screen_types.h"

#include "BLI_listbase.h"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_string.h"
#include "BLI_time.h"

#include "BKE_global.hh"
#include "BKE_context.hh"
#include "BKE_screen.hh"

#include "ED_view3d_offscreen.hh"
#include "ED_screen.hh"
#include "UI_view2d.hh"
#include "DNA_view3d_types.h"

#include "GHOST_Xr-api.hh"

#include "GPU_batch_presets.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_viewport.hh"
#include "GPU_framebuffer.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "MEM_guardedalloc.h"

#include "wm_xr_intern.hh"

namespace blender {
  
extern CLG_LogRef LOG;

struct wmXrPanelSourceContextOverride {
  bContext *C;
  wmWindow *prev_win;
  ScrArea *prev_area;
  ARegion *prev_region;

  wmXrPanelSourceContextOverride(bContext *context, const wmXrPanel *panel) : C(context)
  {
    prev_win = CTX_wm_window(C);
    prev_area = CTX_wm_area(C);
    prev_region = CTX_wm_region(C);

    CTX_wm_window_set(C, panel->panel_source_win);
    CTX_wm_area_set(C, panel->panel_source_area);
    CTX_wm_region_set(C, panel->panel_source_region);
  }

  ~wmXrPanelSourceContextOverride()
  {
    CTX_wm_window_set(C, prev_win);
    CTX_wm_area_set(C, prev_area);
    CTX_wm_region_set(C, prev_region);
  }
};

static wmXrPanel *wm_xr_panel_find(wmXrSurfaceData *surface_data,
                                   const wmWindow *win,
                                   const ScrArea *area)
{
  if (surface_data == nullptr) {
    return nullptr;
  }

  for (wmXrPanel *panel : ListBaseWrapper<wmXrPanel>(surface_data->panels)) {
    if (panel->panel_source_win == win && panel->panel_source_area == area) {
      return panel;
    }
  }
  return nullptr;
}

static void wm_xr_panel_default_transform_init(wmXrPanel *panel)
{
  const float half_pi = 3.1415f * 0.5f;
  const float screen_to_world_scale = 1.0f / 256.0f;
  float pos[3] = {0.0f, 0.0f, 2.0f};
  float rot[3] = {half_pi, 0.0f, 0.0f};
  float size[3] = {screen_to_world_scale, screen_to_world_scale, screen_to_world_scale};
  loc_eul_size_to_mat4(panel->panel_obmat, pos, rot, size);
}

static wmXrPanel *wm_xr_panel_ensure(wmXrSurfaceData *surface_data,
                                     wmWindow *win,
                                     ScrArea *area,
                                     ARegion *region)
{
  wmXrPanel *panel = wm_xr_panel_find(surface_data, win, area);
  if (panel != nullptr) {
    panel->panel_source_region = region;
    return panel;
  }

  panel = MEM_new_zeroed<wmXrPanel>(__func__);
  wm_xr_panel_default_transform_init(panel);
  panel->panel_frame_tag = surface_data->panels_frame_tag;
  panel->panel_source_win = win;
  panel->panel_source_area = area;
  panel->panel_source_region = region;
  BLI_addtail(&surface_data->panels, panel);
  return panel;
}

static void wm_xr_panel_pointer_clear(wmXrPanel *panel)
{
  panel->panel_hovered = false;
  panel->panel_cursor_visible = false;
  panel->panel_pointer.pressed = false;
  panel->panel_pointer.subaction_path[0] = '\0';
  panel->panel_pointer.action_idname[0] = '\0';
}

static void wm_xr_panel_cursor_draw(const wmXrSurfaceData *surface_data)
{
  if (surface_data == nullptr) {
    return;
  }

  const float cursor_color[4] = {1.0f, 1.0f, 1.0f, 0.9f};
  gpu::Batch *sphere = GPU_batch_preset_sphere(3);
  GPU_batch_program_set_builtin(sphere, GPU_SHADER_3D_UNIFORM_COLOR);
  GPU_batch_uniform_4fv(sphere, "color", cursor_color);
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPU_blend(GPU_BLEND_ALPHA);

  for (const wmXrPanel *panel : ConstListBaseWrapper<wmXrPanel>(surface_data->panels)) {
    if (!panel->panel_cursor_visible) {
      continue;
    }
    GPU_matrix_push();
    GPU_matrix_translate_3fv(panel->panel_cursor_world);
    GPU_matrix_scale_1f(0.0075f);
    GPU_batch_draw(sphere);
    GPU_matrix_pop();
  }
}

static void wm_xr_draw_cached_panel_overlay(const float viewmat[4][4],
                                            const float winmat[4][4],
                                            const wmXrSurfaceData *surface_data)
{
  if (surface_data == nullptr) {
    return;
  }

  for (const wmXrPanel *panel : ConstListBaseWrapper<wmXrPanel>(surface_data->panels)) {
    if (!panel->panel_valid || panel->panel_offscreen == nullptr) {
      continue;
    }

    RegionView3D rv_tmp = {};
    copy_m4_m4(rv_tmp.winmat, winmat);
    copy_m4_m4(rv_tmp.viewmat, viewmat);
    ED_region_panels_draw_to_world_quad(
        &rv_tmp, panel->panel_obmat, &panel->panel_rect, panel->panel_offscreen);
  }
}

static const wmXrController *wm_xr_surface_interaction_controller_find(const wmXrSessionState *state,
                                                                       char *r_subaction_path)
{
  const wmXrController *fallback = nullptr;
  for (const wmXrController &controller : state->controllers) {
    if (!controller.aim_active) {
      continue;
    }
    if (strstr(controller.subaction_path, "right") != nullptr) {
      BLI_strncpy(r_subaction_path, controller.subaction_path, XR_MAX_USER_PATH_LENGTH);
      return &controller;
    }
    if (fallback == nullptr) {
      fallback = &controller;
    }
  }
  if (fallback != nullptr) {
    BLI_strncpy(r_subaction_path, fallback->subaction_path, XR_MAX_USER_PATH_LENGTH);
  }
  return fallback;
}

static bool wm_xr_surface_action_is_teleport(const wmXrAction *action)
{
  return action != nullptr && action->ot != nullptr && action->ot->idname != nullptr &&
         BLI_strcasestr(action->ot->idname, "xr_navigation_teleport") != nullptr;
}

static bool wm_xr_surface_controller_teleport_active(const wmXrData *xr, const char *subaction_path)
{
  if (xr == nullptr || xr->runtime == nullptr || subaction_path == nullptr) {
    return false;
  }

  const wmXrActionSet *active_action_set = xr->runtime->session_state.active_action_set;
  if (active_action_set == nullptr) {
    return false;
  }

  for (LinkData &ld : active_action_set->active_modal_actions) {
    const wmXrAction *action = static_cast<const wmXrAction *>(ld.data);
    if (wm_xr_surface_action_is_teleport(action) && action->active_modal_path != nullptr &&
        STREQ(action->active_modal_path, subaction_path))
    {
      return true;
    }
  }

  return false;
}

static void wm_xr_surface_interaction_ray_from_pose(const GHOST_XrPose *aim_pose,
                                                    float r_origin[3],
                                                    float r_direction[3])
{
  float aim_mat[4][4];
  wm_xr_pose_to_mat(aim_pose, aim_mat);
  copy_v3_v3(r_origin, aim_pose->position);
  normalize_v3_v3(r_direction, aim_mat[2]);
  negate_v3(r_direction);
}

static bool wm_xr_surface_interaction_raycast(const wmXrPanel *panel,
                                              const float ray_origin[3],
                                              const float ray_direction[3],
                                              int r_region_xy[2],
                                              float r_hit_world[3],
                                              float *r_lambda)
{
  if (panel == nullptr || !panel->panel_valid || panel->panel_offscreen == nullptr) {
    return false;
  }

  float obimat[4][4];
  if (!invert_m4_m4(obimat, panel->panel_obmat)) {
    return false;
  }

  float ray_end[3];
  madd_v3_v3v3fl(ray_end, ray_origin, ray_direction, 100.0f);

  float origin_local[3], end_local[3], dir_local[3];
  copy_v3_v3(origin_local, ray_origin);
  copy_v3_v3(end_local, ray_end);
  mul_m4_v3(obimat, origin_local);
  mul_m4_v3(obimat, end_local);
  sub_v3_v3v3(dir_local, end_local, origin_local);

  if (fabsf(dir_local[2]) < 1e-6f) {
    return false;
  }

  const float lambda = -origin_local[2] / dir_local[2];
  if (lambda < 0.0f) {
    return false;
  }

  float hit_local[3];
  madd_v3_v3v3fl(hit_local, origin_local, dir_local, lambda);

  const int width = BLI_rcti_size_x(&panel->panel_rect) + 1;
  const int height = BLI_rcti_size_y(&panel->panel_rect) + 1;
  if (hit_local[0] < 0.0f || hit_local[1] < 0.0f || hit_local[0] > width || hit_local[1] > height) {
    return false;
  }

  r_region_xy[0] = panel->panel_rect.xmin + round_fl_to_int(hit_local[0]);
  r_region_xy[1] = panel->panel_rect.ymin + round_fl_to_int(hit_local[1]);
  madd_v3_v3v3fl(r_hit_world, ray_origin, ray_direction, 100.0f * lambda);
  if (r_lambda != nullptr) {
    *r_lambda = lambda;
  }
  return true;
}

static void wm_xr_surface_interaction_event_add(wmWindow *win, short type, short val, const int xy[2])
{
  wmEvent event{};
  wm_event_init_from_window(win, &event);
  copy_v2_v2_int(event.xy, xy);
  copy_v2_v2_int(event.prev_xy, win->runtime->eventstate->xy);
  event.type = static_cast<wmEventType>(type);
  event.val = val;

  const int g_flag_prev = G.f;
  G.f |= G_FLAG_EVENT_SIMULATE;
  WM_event_add_simulate(win, &event);
  G.f = g_flag_prev;
}

static bool wm_xr_panel_cache_update(const bContext *C, wmXrPanel *panel)
{
  if (panel != nullptr && panel->panel_valid && panel->panel_offscreen != nullptr && !panel->panel_dirty &&
      panel->panel_last_rebuild_tag == panel->panel_frame_tag)
  {
    return true;
  }

  ScrArea *area = CTX_wm_area(C);
  ARegion *ui_region = area ? BKE_area_find_region_type(area, RGN_TYPE_UI) : nullptr;
  if (panel == nullptr || area == nullptr || ui_region == nullptr || ui_region->runtime == nullptr ||
      ui_region->runtime->type == nullptr)
  {
    return false;
  }

  short prev_alignment;
  ARegion *prev_region = nullptr;
  rcti panel_rect;
  bContext *mutable_C = const_cast<bContext *>(C);
  ED_region_panels_world_layout_begin(mutable_C, ui_region, &panel_rect, &prev_alignment, &prev_region);

  const int w = BLI_rcti_size_x(&panel_rect) + 1;
  const int h = BLI_rcti_size_y(&panel_rect) + 1;
  bool create_new = true;
  if (panel->panel_offscreen) {
    if (GPU_offscreen_width(panel->panel_offscreen) == w &&
        GPU_offscreen_height(panel->panel_offscreen) == h)
    {
      create_new = false;
    }
    else {
      GPU_offscreen_free(panel->panel_offscreen);
      panel->panel_offscreen = nullptr;
    }
  }
  if (create_new) {
    panel->panel_offscreen = GPU_offscreen_create(
        w, h, false, gpu::TextureFormat::UNORM_8_8_8_8, GPU_TEXTURE_USAGE_SHADER_READ, false, nullptr);
  }
  if (!panel->panel_offscreen) {
    CLOG_ERROR(&LOG, "panels_ws: offscreen create failed");
    ED_region_panels_world_layout_end(mutable_C, ui_region, prev_alignment, prev_region);
    return false;
  }

  ED_region_panels_draw_offscreen(C, ui_region, &panel_rect, panel->panel_offscreen);
  panel->panel_rect = panel_rect;
  panel->panel_valid = true;
  panel->panel_dirty = false;
  panel->panel_last_rebuild_tag = panel->panel_frame_tag;
  panel->panel_source_win = CTX_wm_window(C);
  panel->panel_source_area = area;
  panel->panel_source_region = ui_region;

  ED_region_panels_world_layout_end(mutable_C, ui_region, prev_alignment, prev_region);
  return true;
}

static void wm_xr_panel_cache_refresh_source(const bContext *C, wmXrPanel *panel)
{
  if (C == nullptr || panel == nullptr || panel->panel_source_win == nullptr ||
      panel->panel_source_area == nullptr || panel->panel_source_region == nullptr)
  {
    return;
  }

  bContext *mutable_C = const_cast<bContext *>(C);
  wmXrPanelSourceContextOverride context_override(mutable_C, panel);
  wm_xr_panel_cache_update(mutable_C, panel);
  ED_region_tag_redraw(panel->panel_source_region);
}

void wm_xr_surface_interaction_update(const bContext *C, wmXrData *xr)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (C == nullptr || xr == nullptr || surface_data == nullptr)
  {
    return;
  }

  char subaction_path[64] = "";
  const wmXrController *controller = wm_xr_surface_interaction_controller_find(
      &xr->runtime->session_state, subaction_path);
  if (controller == nullptr) {
    if (surface_data->active_panel != nullptr && !surface_data->active_panel->panel_pointer.pressed) {
      wm_xr_panel_pointer_clear(surface_data->active_panel);
      surface_data->active_panel = nullptr;
    }
    return;
  }

  const bool is_captured_panel_drag = surface_data->active_panel != nullptr &&
                                      surface_data->active_panel->panel_pointer.pressed &&
                                      STREQ(surface_data->active_panel->panel_pointer.subaction_path,
                                            subaction_path);
  if (!is_captured_panel_drag && wm_xr_surface_controller_teleport_active(xr, subaction_path)) {
    if (surface_data->active_panel != nullptr && !surface_data->active_panel->panel_pointer.pressed) {
      wm_xr_panel_pointer_clear(surface_data->active_panel);
      surface_data->active_panel = nullptr;
    }
    return;
  }

  float ray_origin[3], ray_direction[3];
  wm_xr_surface_interaction_ray_from_pose(&controller->aim_pose, ray_origin, ray_direction);
  wmXrPanel *hit_panel = nullptr;
  int hit_region_xy[2] = {0, 0};
  float hit_world[3] = {0.0f, 0.0f, 0.0f};
  float hit_lambda = FLT_MAX;

  if (is_captured_panel_drag) {
    hit_panel = surface_data->active_panel;
    if (!wm_xr_surface_interaction_raycast(
            hit_panel, ray_origin, ray_direction, hit_region_xy, hit_world, &hit_lambda))
    {
      return;
    }
  }
  else {
    for (wmXrPanel *panel : ListBaseWrapper<wmXrPanel>(surface_data->panels)) {
      int region_xy[2];
      float panel_hit_world[3];
      float lambda;
      if (!wm_xr_surface_interaction_raycast(
              panel, ray_origin, ray_direction, region_xy, panel_hit_world, &lambda))
      {
        continue;
      }
      if (lambda < hit_lambda) {
        hit_panel = panel;
        hit_lambda = lambda;
        copy_v2_v2_int(hit_region_xy, region_xy);
        copy_v3_v3(hit_world, panel_hit_world);
      }
    }
  }

  if (hit_panel == nullptr) {
    if (surface_data->active_panel != nullptr && !surface_data->active_panel->panel_pointer.pressed) {
      wm_xr_panel_pointer_clear(surface_data->active_panel);
      surface_data->active_panel = nullptr;
    }
    return;
  }

  hit_panel->panel_cursor_visible = true;
  copy_v3_v3(hit_panel->panel_cursor_world, hit_world);

  if (surface_data->active_panel != nullptr && surface_data->active_panel != hit_panel &&
      !surface_data->active_panel->panel_pointer.pressed)
  {
    wm_xr_panel_pointer_clear(surface_data->active_panel);
  }
  surface_data->active_panel = hit_panel;

  if (!hit_panel->panel_hovered || hit_panel->panel_region_xy[0] != hit_region_xy[0] ||
      hit_panel->panel_region_xy[1] != hit_region_xy[1] ||
      !STREQ(hit_panel->panel_pointer.subaction_path, subaction_path))
  {
    int win_xy[2] = {
        hit_panel->panel_source_region->winrct.xmin + hit_region_xy[0],
        hit_panel->panel_source_region->winrct.ymin + hit_region_xy[1],
    };
    wm_xr_surface_interaction_event_add(
        hit_panel->panel_source_win, MOUSEMOVE, KM_NOTHING, win_xy);
    ED_region_tag_redraw(hit_panel->panel_source_region);
    hit_panel->panel_dirty = true;
    wm_xr_panel_cache_refresh_source(C, hit_panel);
  }

  copy_v2_v2_int(hit_panel->panel_region_xy, hit_region_xy);
  hit_panel->panel_hovered = true;
  BLI_strncpy(hit_panel->panel_pointer.subaction_path, subaction_path, XR_MAX_USER_PATH_LENGTH);
}

bool wm_xr_surface_interaction_apply_action(const bContext *C,
                                            wmXrData *xr,
                                            const wmXrAction *action,
                                            const char *subaction_path,
                                            short event_val)
{
  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  wmXrPanel *panel = surface_data ? surface_data->active_panel : nullptr;
  if (C == nullptr || xr == nullptr || action == nullptr || subaction_path == nullptr ||
      surface_data == nullptr || panel == nullptr || panel->panel_source_win == nullptr ||
      panel->panel_source_region == nullptr)
  {
    return false;
  }

  if (!ELEM(event_val, KM_PRESS, KM_RELEASE)) {
    return false;
  }

  /* Only a teleport press that begins over the panel may start capture. After that, only the
   * matching release for that captured action/subaction is rerouted to the panel. */
  if (event_val == KM_PRESS) {
    if (panel->panel_pointer.pressed && action->ot != nullptr &&
        STREQ(panel->panel_pointer.subaction_path, subaction_path) &&
        STREQ(panel->panel_pointer.action_idname, action->ot->idname))
    {
      /* Keep consuming held teleport press events for an active panel drag so the teleport modal
       * path cannot start while the panel interaction owns this controller. */
      return true;
    }
    if (!wm_xr_surface_action_is_teleport(action)) {
      return false;
    }
    if (!panel->panel_hovered || !STREQ(panel->panel_pointer.subaction_path, subaction_path))
    {
      return false;
    }
    int win_xy[2] = {
        panel->panel_source_region->winrct.xmin + panel->panel_region_xy[0],
        panel->panel_source_region->winrct.ymin + panel->panel_region_xy[1],
    };
    wm_xr_surface_interaction_event_add(panel->panel_source_win, LEFTMOUSE, KM_PRESS, win_xy);
    panel->panel_pointer.pressed = true;
    BLI_strncpy(
        panel->panel_pointer.action_idname,
        action->ot->idname,
        sizeof(panel->panel_pointer.action_idname));
    ED_region_tag_redraw(panel->panel_source_region);
    panel->panel_dirty = true;
    wm_xr_panel_cache_refresh_source(C, panel);
    return true;
  }

  if (event_val == KM_RELEASE && panel->panel_pointer.pressed &&
      STREQ(panel->panel_pointer.subaction_path, subaction_path) && action->ot != nullptr &&
      STREQ(panel->panel_pointer.action_idname, action->ot->idname))
  {
    int win_xy[2] = {
        panel->panel_source_region->winrct.xmin + panel->panel_region_xy[0],
        panel->panel_source_region->winrct.ymin + panel->panel_region_xy[1],
    };
    wm_xr_surface_interaction_event_add(panel->panel_source_win, LEFTMOUSE, KM_RELEASE, win_xy);
    wm_xr_panel_pointer_clear(panel);
    surface_data->active_panel = nullptr;
    ED_region_tag_redraw(panel->panel_source_region);
    panel->panel_dirty = true;
    wm_xr_panel_cache_refresh_source(C, panel);
    return true;
  }

  return false;
}

void wm_xr_pose_to_mat(const GHOST_XrPose *pose, float r_mat[4][4])
{
  quat_to_mat4(r_mat, pose->orientation_quat);
  copy_v3_v3(r_mat[3], pose->position);
}

void wm_xr_pose_scale_to_mat(const GHOST_XrPose *pose, float scale, float r_mat[4][4])
{
  wm_xr_pose_to_mat(pose, r_mat);

  BLI_assert(scale > 0.0f);
  mul_v3_fl(r_mat[0], scale);
  mul_v3_fl(r_mat[1], scale);
  mul_v3_fl(r_mat[2], scale);
}

void wm_xr_pose_to_imat(const GHOST_XrPose *pose, float r_imat[4][4])
{
  float iquat[4];
  invert_qt_qt_normalized(iquat, pose->orientation_quat);
  quat_to_mat4(r_imat, iquat);
  translate_m4(r_imat, -pose->position[0], -pose->position[1], -pose->position[2]);
}

void wm_xr_pose_scale_to_imat(const GHOST_XrPose *pose, float scale, float r_imat[4][4])
{
  float iquat[4];
  invert_qt_qt_normalized(iquat, pose->orientation_quat);
  quat_to_mat4(r_imat, iquat);

  BLI_assert(scale > 0.0f);
  scale = 1.0f / scale;
  mul_v3_fl(r_imat[0], scale);
  mul_v3_fl(r_imat[1], scale);
  mul_v3_fl(r_imat[2], scale);

  translate_m4(r_imat, -pose->position[0], -pose->position[1], -pose->position[2]);
}

static void wm_xr_draw_matrices_create(const wmXrDrawData *draw_data,
                                       const GHOST_XrDrawViewInfo *draw_view,
                                       const XrSessionSettings *session_settings,
                                       const wmXrSessionState *session_state,
                                       float r_viewmat[4][4],
                                       float r_projmat[4][4])
{
  GHOST_XrPose eye_pose;
  float eye_inv[4][4], base_inv[4][4], nav_inv[4][4], m[4][4];

  /* Calculate inverse eye matrix. */
  copy_qt_qt(eye_pose.orientation_quat, draw_view->eye_pose.orientation_quat);
  copy_v3_v3(eye_pose.position, draw_view->eye_pose.position);
  if ((session_settings->flag & XR_SESSION_USE_POSITION_TRACKING) == 0) {
    sub_v3_v3(eye_pose.position, draw_view->local_pose.position);
  }
  if ((session_settings->flag & XR_SESSION_USE_ABSOLUTE_TRACKING) == 0) {
    sub_v3_v3(eye_pose.position, draw_data->eye_position_ofs);
  }

  wm_xr_pose_to_imat(&eye_pose, eye_inv);

  /* Apply base pose and navigation. */
  wm_xr_pose_scale_to_imat(&draw_data->base_pose, draw_data->base_scale, base_inv);
  wm_xr_pose_scale_to_imat(&session_state->nav_pose_last_actions_sync,
                           session_state->viewer_scale_last_actions_sync,
                           nav_inv);
  mul_m4_m4m4(m, eye_inv, base_inv);
  mul_m4_m4m4(r_viewmat, m, nav_inv);

  perspective_m4_fov(r_projmat,
                     draw_view->fov.angle_left,
                     draw_view->fov.angle_right,
                     draw_view->fov.angle_up,
                     draw_view->fov.angle_down,
                     session_settings->clip_start,
                     session_settings->clip_end);
}

static void wm_xr_draw_viewport_buffers_to_active_framebuffer(
    const wmXrRuntimeData *runtime_data,
    const wmXrSurfaceData *surface_data,
    const GHOST_XrDrawViewInfo *draw_view)
{
  const wmXrViewportPair *vp = static_cast<const wmXrViewportPair *>(
      BLI_findlink(&surface_data->viewports, draw_view->view_idx));
  BLI_assert(vp && vp->viewport);

  const bool is_upside_down = GHOST_XrSessionNeedsUpsideDownDrawing(runtime_data->ghost_context);
  rcti rect{};
  rect.xmin = 0;
  rect.ymin = 0;
  rect.xmax = draw_view->width - 1;
  rect.ymax = draw_view->height - 1;

  wmViewport(&rect);

  /* For upside down contexts, draw with inverted y-values. */
  if (is_upside_down) {
    std::swap(rect.ymin, rect.ymax);
  }
  GPU_viewport_draw_to_screen_ex(vp->viewport, 0, &rect, draw_view->expects_srgb_buffer, true);
}

void wm_xr_draw_view(const GHOST_XrDrawViewInfo *draw_view, void *customdata)
{
  wmXrDrawData *draw_data = static_cast<wmXrDrawData *>(customdata);
  wmXrData *xr_data = draw_data->xr_data;
  wmXrSurfaceData *surface_data = draw_data->surface_data;
  wmXrSessionState *session_state = &xr_data->runtime->session_state;
  XrSessionSettings *settings = &xr_data->session_settings;

  const int display_flags = V3D_OFSDRAW_OVERRIDE_SCENE_SETTINGS | settings->draw_flags;

  float viewmat[4][4], winmat[4][4];

  BLI_assert(WM_xr_session_is_ready(xr_data));

  wm_xr_session_draw_data_update(session_state, settings, draw_view, draw_data);
  wm_xr_draw_matrices_create(draw_data, draw_view, settings, session_state, viewmat, winmat);
  wm_xr_session_state_update(settings, draw_data, draw_view, session_state);

  if (!wm_xr_session_surface_offscreen_ensure(surface_data, draw_view)) {
    return;
  }

  const wmXrViewportPair *vp = static_cast<const wmXrViewportPair *>(
      BLI_findlink(&surface_data->viewports, draw_view->view_idx));
  BLI_assert(vp && vp->offscreen && vp->viewport);

  /* In case a framebuffer is still bound from drawing the last eye. */
  GPU_framebuffer_restore();
  /* Some systems have drawing glitches without this. */
  GPU_clear_depth(1.0f);

  /* XR context is ensured before each draw in #wm_xr_session_surface_draw. */
  bContext *xr_context = WM_xr_session_context_get(xr_data);
  Scene *scene = CTX_data_scene(xr_context);

  /* The XR context depsgraph is separately evaluated outside of drawing within the XR surface
   * #do_depsgraph callback. Thus, obtain the depsgraph directly without evaluating it. */
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(xr_context);

  if (draw_view->view_idx == 0) {
    /* Only render location scouting viewfinder on first eye draw. */
    wm_xr_viewfinder_render_view(xr_data);
  }

  /* Draws the view into the surface_data->viewport's frame-buffers. */
  ED_view3d_draw_offscreen_simple(depsgraph,
                                  scene,
                                  &settings->shading,
                                  xr_context,
                                  eDrawType(settings->shading.type),
                                  settings->object_type_exclude_viewport,
                                  settings->object_type_exclude_select,
                                  draw_view->width,
                                  draw_view->height,
                                  display_flags,
                                  viewmat,
                                  winmat,
                                  settings->clip_start,
                                  settings->clip_end,
                                  session_state->vignette_aperture,
                                  true,
                                  false,
                                  true,
                                  nullptr,
                                  false,
                                  nullptr,
                                  vp->offscreen,
                                  vp->viewport);

  /* The draw-manager uses both GPUOffscreen and GPUViewport to manage frame and texture buffers. A
   * call to GPU_viewport_draw_to_screen() is still needed to get the final result from the
   * viewport buffers composited together and potentially color managed for display on screen.
   * It needs a bound frame-buffer to draw into, for which we simply reuse the GPUOffscreen one.
   *
   * In a next step, Ghost-XR will use the currently bound frame-buffer to retrieve the image
   * to be submitted to the OpenXR swap-chain. So do not un-bind the off-screen yet! */

  GPU_offscreen_bind(vp->offscreen, false);

  wm_xr_draw_viewport_buffers_to_active_framebuffer(xr_data->runtime, surface_data, draw_view);

  if ((settings->draw_flags & V3D_OFSDRAW_XR_SHOW_CUSTOM_OVERLAYS) != 0) {
    /* wm_xr_draw_cached_panel_overlay requires an inverted y-axis to function properly. */
    for (uint i = 0; i < 4; ++i) {
      viewmat[i][1] *= -1.0f;
    }

    wm_xr_draw_cached_panel_overlay(viewmat, winmat, surface_data);
  }
}

bool wm_xr_passthrough_enabled(void *customdata)
{
  wmXrDrawData *draw_data = static_cast<wmXrDrawData *>(customdata);
  wmXrData *xr_data = draw_data->xr_data;
  XrSessionSettings *settings = &xr_data->session_settings;

  return (settings->draw_flags & V3D_OFSDRAW_XR_SHOW_PASSTHROUGH) != 0;
}

void wm_xr_disable_passthrough(void *customdata)
{
  wmXrDrawData *draw_data = static_cast<wmXrDrawData *>(customdata);
  wmXrData *xr_data = draw_data->xr_data;
  XrSessionSettings *settings = &xr_data->session_settings;

  settings->draw_flags &= ~V3D_OFSDRAW_XR_SHOW_PASSTHROUGH;
  WM_global_report(RPT_INFO, "Passthrough not available");
}

static gpu::Batch *wm_xr_controller_model_batch_create(GHOST_IXrContext *xr_context,
                                                       const char *subaction_path)
{
  GHOST_XrControllerModelData model_data;

  if (!GHOST_XrGetControllerModelData(xr_context, subaction_path, &model_data) ||
      model_data.count_vertices < 1)
  {
    return nullptr;
  }

  GPUVertFormat format = {0};
  GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  GPU_vertformat_attr_add(&format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);

  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, model_data.count_vertices);
  vbo->data<GHOST_XrControllerModelVertex>().copy_from(
      {model_data.vertices, model_data.count_vertices});

  gpu::IndexBuf *ibo = nullptr;
  if (model_data.count_indices > 0 && ((model_data.count_indices % 3) == 0)) {
    GPUIndexBufBuilder ibo_builder;
    const uint prim_len = model_data.count_indices / 3;
    GPU_indexbuf_init(&ibo_builder, GPU_PRIM_TRIS, prim_len, model_data.count_vertices);
    for (uint i = 0; i < prim_len; ++i) {
      const uint32_t *idx = &model_data.indices[i * 3];
      GPU_indexbuf_add_tri_verts(&ibo_builder, idx[0], idx[1], idx[2]);
    }
    ibo = GPU_indexbuf_build(&ibo_builder);
  }

  return GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, ibo, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
}

static void wm_xr_controller_model_draw(const XrSessionSettings *settings,
                                        GHOST_IXrContext *xr_context,
                                        wmXrSessionState *state)
{
  GHOST_XrControllerModelData model_data;

  float color[4];
  switch (settings->controller_draw_style) {
    case XR_CONTROLLER_DRAW_DARK:
    case XR_CONTROLLER_DRAW_DARK_RAY:
      color[0] = color[1] = color[2] = 0.0f;
      color[3] = 0.4f;
      break;
    case XR_CONTROLLER_DRAW_LIGHT:
    case XR_CONTROLLER_DRAW_LIGHT_RAY:
      color[0] = 0.422f;
      color[1] = 0.438f;
      color[2] = 0.446f;
      color[3] = 0.4f;
      break;
  }

  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_blend(GPU_BLEND_ALPHA);

  for (wmXrController &controller : state->controllers) {
    if (!controller.grip_active) {
      continue;
    }

    gpu::Batch *model = controller.model;
    if (!model) {
      model = controller.model = wm_xr_controller_model_batch_create(xr_context,
                                                                     controller.subaction_path);
    }

    if (model &&
        GHOST_XrGetControllerModelData(xr_context, controller.subaction_path, &model_data) &&
        model_data.count_components > 0)
    {
      GPU_batch_program_set_builtin(model, GPU_SHADER_3D_UNIFORM_COLOR);
      GPU_batch_uniform_4fv(model, "color", color);

      GPU_matrix_push();
      GPU_matrix_mul(controller.grip_mat);
      for (uint component_idx = 0; component_idx < model_data.count_components; ++component_idx) {
        const GHOST_XrControllerModelComponent *component = &model_data.components[component_idx];
        GPU_matrix_push();
        GPU_matrix_mul(component->transform);
        GPU_batch_draw_range(model,
                             model->elem ? component->index_offset : component->vertex_offset,
                             model->elem ? component->index_count : component->vertex_count);
        GPU_matrix_pop();
      }
      GPU_matrix_pop();
    }
    else {
      /* Fallback. */
      const float scale = 0.05f;
      gpu::Batch *sphere = GPU_batch_preset_sphere(2);
      GPU_batch_program_set_builtin(sphere, GPU_SHADER_3D_UNIFORM_COLOR);
      GPU_batch_uniform_4fv(sphere, "color", color);

      GPU_matrix_push();
      GPU_matrix_mul(controller.grip_mat);
      GPU_matrix_scale_1f(scale);
      GPU_batch_draw(sphere);
      GPU_matrix_pop();
    }
  }
}

static void wm_xr_controller_aim_draw(const XrSessionSettings *settings, wmXrSessionState *state)
{
  const bool draw_ray = ELEM(
      settings->controller_draw_style, XR_CONTROLLER_DRAW_DARK_RAY, XR_CONTROLLER_DRAW_LIGHT_RAY);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  uint col = GPU_vertformat_attr_add(format, "color", gpu::VertAttrType::SFLOAT_32_32_32_32);
  immBindBuiltinProgram(GPU_SHADER_3D_POLYLINE_FLAT_COLOR);

  float viewport[4];
  GPU_viewport_size_get_f(viewport);
  immUniform2fv("viewportSize", &viewport[2]);

  immUniform1f("lineWidth", 3.0f * U.pixelsize);

  if (draw_ray) {
    const float color[4] = {0.33f, 0.33f, 1.0f, 0.5f};
    const float scale = settings->clip_end;
    float ray[3];

    GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
    GPU_blend(GPU_BLEND_ALPHA);

    for (wmXrController &controller : state->controllers) {
      if (!controller.grip_active) {
        continue;
      }

      immBegin(GPU_PRIM_LINES, 2);

      const float (*mat)[4] = controller.aim_mat;
      madd_v3_v3v3fl(ray, mat[3], mat[2], -scale);

      immAttrSkip(col);
      immVertex3fv(pos, mat[3]);
      immAttr4fv(col, color);
      immVertex3fv(pos, ray);

      immEnd();
    }
  }
  else {
    const float r[4] = {255 / 255.0f, 51 / 255.0f, 82 / 255.0f, 255 / 255.0f};
    const float g[4] = {139 / 255.0f, 220 / 255.0f, 0 / 255.0f, 255 / 255.0f};
    const float b[4] = {40 / 255.0f, 144 / 255.0f, 255 / 255.0f, 255 / 255.0f};
    const float scale = 0.01f;
    float x_axis[3], y_axis[3], z_axis[3];

    GPU_depth_test(GPU_DEPTH_NONE);
    GPU_blend(GPU_BLEND_NONE);

    for (wmXrController &controller : state->controllers) {
      if (!controller.grip_active) {
        continue;
      }

      immBegin(GPU_PRIM_LINES, 6);

      const float (*mat)[4] = controller.aim_mat;
      madd_v3_v3v3fl(x_axis, mat[3], mat[0], scale);
      madd_v3_v3v3fl(y_axis, mat[3], mat[1], scale);
      madd_v3_v3v3fl(z_axis, mat[3], mat[2], scale);

      immAttrSkip(col);
      immVertex3fv(pos, mat[3]);
      immAttr4fv(col, r);
      immVertex3fv(pos, x_axis);

      immAttrSkip(col);
      immVertex3fv(pos, mat[3]);
      immAttr4fv(col, g);
      immVertex3fv(pos, y_axis);

      immAttrSkip(col);
      immVertex3fv(pos, mat[3]);
      immAttr4fv(col, b);
      immVertex3fv(pos, z_axis);

      immEnd();
    }
  }

  immUnbindProgram();
}

void wm_xr_draw_controllers(const bContext *C, ARegion * /*region*/, void *customdata)
{
  wmXrData *xr = static_cast<wmXrData *>(customdata);
  const XrSessionSettings *settings = &xr->session_settings;
  GHOST_IXrContext *xr_context = xr->runtime->ghost_context;
  wmXrSessionState *state = &xr->runtime->session_state;

  wm_xr_controller_model_draw(settings, xr_context, state);
  wm_xr_controller_aim_draw(settings, state);
  wm_xr_viewfinder_draw(C, settings, state);
  wm_xr_panel_cursor_draw(WM_xr_surface_data_get());
}

static CLG_LogRef LOG = {"xr"};

void wm_xr_draw_panels_world_space(const bContext * C, ARegion * region, void *customdata)
{
  if (region == nullptr) {
    CLOG_ERROR(&LOG, "panels_ws: skipped, null region");
    return;
  }
  if (C == nullptr) {
    CLOG_ERROR(&LOG, "panels_ws: skipped, null context");
    return;
  }
  BLI_assert(customdata != nullptr);

  wmXrData *xr = static_cast<wmXrData *>(customdata);
  const XrSessionSettings *settings = &xr->session_settings;
  if ((settings->draw_flags & V3D_OFSDRAW_XR_SHOW_CUSTOM_OVERLAYS) == 0) {
    CLOG_ERROR(&LOG, "panels_ws: custom overlays disabled");
    return;
  }
  if (region->regiontype != RGN_TYPE_WINDOW) {
    CLOG_ERROR(&LOG, "panels_ws: wrong region type (%d)", region->regiontype);
    return;
  }

  wmXrSurfaceData *surface_data = WM_xr_surface_data_get();
  if (!surface_data) {
    return;
  }

  ScrArea *area = CTX_wm_area(C);
  ARegion *ui_region = area ? BKE_area_find_region_type(area, RGN_TYPE_UI) : nullptr;
  if (area == nullptr || ui_region == nullptr) {
    return;
  }

  wmXrPanel *panel = wm_xr_panel_ensure(surface_data, CTX_wm_window(C), area, ui_region);
  wm_xr_panel_cache_update(C, panel);
}

}  // namespace blender
