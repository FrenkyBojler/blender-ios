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
#include <fmt/format.h>

#include "DNA_camera_types.h"
#include "DNA_userdef_types.h"

#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_rect.h"
#include "BLI_time.h"

#include "BKE_camera.h"
#include "BKE_context.hh"

#include "ED_view3d_offscreen.hh"

#include "GHOST_Xr-api.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "GPU_batch_presets.hh"
#include "GPU_framebuffer.hh"
#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_viewport.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "MEM_guardedalloc.h"

#include "UI_resources.hh"

#include "WM_api.hh"

#include "wm_xr_intern.hh"

namespace blender {

extern bContext *evil_main_C;
static GPUOffScreen *g_viewfinder_offscreen;

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

static wmXrController *get_viewfinder_controller(const XrSessionSettings *settings,
                                                 wmXrSessionState *state)
{
  const char *subaction_path;

  switch (settings->controller_dominant_hand) {
    /* Place the Viewfinder on the non-dominant hand (invert left/right). */
    case XR_CONTROLLER_DHAND_LEFT:
      subaction_path = "/user/hand/right";
      break;
    case XR_CONTROLLER_DHAND_RIGHT:
      subaction_path = "/user/hand/left";
      break;
    default:
      BLI_assert_unreachable();
      return nullptr;
  }

  for (wmXrController &controller : state->controllers) {
    if (STREQ(controller.subaction_path, subaction_path)) {
      return &controller;
    }
  }

  return nullptr;
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
  wm_xr_pose_scale_to_imat(&session_state->nav_pose_prev, session_state->nav_scale_prev, nav_inv);
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

  /* WIP Hack: Draw the viewfinder view here and pass it to wm_xr_controller_model_draw via a
   *           static local global as context prevents us from doing this in model_draw */
  if (g_viewfinder_offscreen == nullptr) {
    char err_out[256] = "unknown";
    g_viewfinder_offscreen = GPU_offscreen_create(draw_view->width,
                                                  draw_view->height,
                                                  true,
                                                  blender::gpu::TextureFormat::UNORM_8_8_8_8,
                                                  GPU_TEXTURE_USAGE_SHADER_READ |
                                                      GPU_TEXTURE_USAGE_MEMORY_EXPORT,
                                                  false,
                                                  err_out);
  }
  static GPUViewport *gpu_viewport = GPU_viewport_create();

  Scene *scene = draw_data->scene;
  Object *camera_ob = scene->camera; /* Active scene camera. */
  Camera *camera_data = id_cast<Camera *>(camera_ob->data);

  /* Hack: The DoF live DoF settings need to be overriden during playback to display
   * the DoF of the captured shot. Circumvent this by storing the live DoF setting
   * when entering playback, and restoring them when going back to live. */
  static bool dirty_dof_settings = false;
  static CameraDOFSettings live_dof_settings = camera_data->dof;

  float viewfinder_viewmat[4][4] = {};
  float current_landmark_vf_lens = 0.0f;
  switch (settings->viewfinder_active_mode) {
    case XR_VIEWFINDER_MODE_LIVE: {
      const wmXrController *viewfinder_controller = get_viewfinder_controller(settings,
                                                                              session_state);
      if (!viewfinder_controller) {
        break;
      }

      if (dirty_dof_settings) {
        camera_data->dof = live_dof_settings;
        dirty_dof_settings = false;
      }

      /* Note: View offsets can be configured using the Scene Camera Shift X/Y settings. */
      float viewfinder_mat[4][4];
      copy_m4_m4(viewfinder_mat, viewfinder_controller->grip_mat);
      rotate_m4(viewfinder_mat, 'X', -M_PI_2);

      invert_m4_m4(viewfinder_viewmat, viewfinder_mat);

      /* Store the last known position/rotation in the XR session state for landmark capture.
       * Note: We really shouldn't mutate runtime data from a drawing function, but this is by
       *       far the simplest way to do it. */
      mat4_to_loc_quat(session_state->viewfinder_position,
                       session_state->viewfinder_orientation_quat,
                       viewfinder_mat);

      break;
    }
    case XR_VIEWFINDER_MODE_PLAYBACK: {
      if (!dirty_dof_settings) {
        live_dof_settings = camera_data->dof;
        dirty_dof_settings = true;
      }

      PointerRNA scene_ptr = RNA_id_pointer_create(&scene->id);

      /* Note: unsafe, relies on the VR add-on to be loaded. */
      PropertyRNA *landmarks_prop = RNA_struct_find_property(&scene_ptr, "vr_landmarks");
      PropertyRNA *landmark_idx_prop = RNA_struct_find_property(&scene_ptr,
                                                                "vr_landmarks_selected");
      const int landmark_idx = RNA_property_int_get(&scene_ptr, landmark_idx_prop);

      /* Hack: Doing some hardcore RNA introspection to obtain the values back. */
      PointerRNA current_landmark;
      RNA_property_collection_lookup_int(
          &scene_ptr, landmarks_prop, landmark_idx, &current_landmark);

      /* Captured pose (location / orientation). */
      PropertyRNA *lm_vf_pos_prop = RNA_struct_find_property(&current_landmark,
                                                             "base_pose_location");
      PropertyRNA *lm_vf_quat_prop = RNA_struct_find_property(&current_landmark,
                                                              "viewfinder_quat");
      float landmark_viewfinder_pos[3];
      float landmark_viewfinder_quat[4];
      RNA_property_float_get_array(&current_landmark, lm_vf_pos_prop, landmark_viewfinder_pos);
      RNA_property_float_get_array(&current_landmark, lm_vf_quat_prop, landmark_viewfinder_quat);

      GHOST_XrPose viewfinder_pose;
      copy_v3_v3(viewfinder_pose.position, landmark_viewfinder_pos);
      copy_qt_qt(viewfinder_pose.orientation_quat, landmark_viewfinder_quat);

      wm_xr_pose_to_imat(&viewfinder_pose, viewfinder_viewmat);

      /* Captured view settings (lens / DoF). */
      PropertyRNA *lm_vf_lens_prop = RNA_struct_find_property(&current_landmark,
                                                              "viewfinder_lens");
      PropertyRNA *lm_vf_use_dof_prop = RNA_struct_find_property(&current_landmark,
                                                                 "viewfinder_use_dof");
      PropertyRNA *lm_vf_dof_dist_prop = RNA_struct_find_property(&current_landmark,
                                                                  "viewfinder_dof_dist");
      PropertyRNA *lm_vf_dof_fstop_prop = RNA_struct_find_property(&current_landmark,
                                                                   "viewfinder_dof_fstop");

      current_landmark_vf_lens = RNA_property_float_get(&current_landmark, lm_vf_lens_prop);
      const bool landmark_use_dof = RNA_property_boolean_get(&current_landmark,
                                                             lm_vf_use_dof_prop);
      SET_FLAG_FROM_TEST(camera_data->dof.flag, landmark_use_dof, CAM_DOF_ENABLED);
      camera_data->dof.focus_distance = RNA_property_float_get(&current_landmark,
                                                               lm_vf_dof_dist_prop);
      camera_data->dof.aperture_fstop = RNA_property_float_get(&current_landmark,
                                                               lm_vf_dof_fstop_prop);
      break;
    }
    default:
      BLI_assert_unreachable();
      break;
  }

  CameraParams params;
  BKE_camera_params_init(&params);
  BKE_camera_params_from_object(&params, camera_ob);

  /* In Playback mode, override the lens with the value stored in the landmark.
   * Note: Only the lens and DoF are restored, tweaking the Shift X/Y and other Camera settings
   *       between captures will cause inconsistencies. */
  if (settings->viewfinder_active_mode == XR_VIEWFINDER_MODE_PLAYBACK) {
    params.lens = current_landmark_vf_lens;
  }

  BKE_camera_params_compute_viewplane(
      &params, scene->r.xsch, scene->r.ysch, scene->r.xasp, scene->r.yasp);
  BKE_camera_params_compute_matrix(&params);

  float viewfinder_winmat[4][4];
  copy_m4_m4(viewfinder_winmat, params.winmat);

  const int viewfinder_display_flag = V3D_OFSDRAW_OVERRIDE_SCENE_SETTINGS |
                                      V3D_OFSDRAW_SHOW_ANNOTATION | V3D_OFSDRAW_SHOW_GRIDFLOOR;

  /* Always enable DoF in the View3D settings used by in the viewfinder rendered view
   * for Workbench. */
  View3DShading viewfinder_shading_settings = settings->shading;
  viewfinder_shading_settings.flag |= V3D_SHADING_DEPTH_OF_FIELD;

  ED_view3d_draw_offscreen_simple(draw_data->depsgraph,
                                  draw_data->scene,
                                  &viewfinder_shading_settings,
                                  (eDrawType)settings->shading.type,
                                  settings->object_type_exclude_viewport,
                                  settings->object_type_exclude_select,
                                  draw_view->width,
                                  draw_view->height,
                                  viewfinder_display_flag,
                                  viewfinder_viewmat,
                                  viewfinder_winmat,
                                  settings->clip_start,
                                  settings->clip_end,
                                  1.0f,
                                  true,
                                  false,
                                  true,
                                  nullptr,
                                  true,
                                  true,
                                  g_viewfinder_offscreen,
                                  gpu_viewport);

  /* Draws the view into the surface_data->viewport's frame-buffers. */
  ED_view3d_draw_offscreen_simple(draw_data->depsgraph,
                                  draw_data->scene,
                                  &settings->shading,
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
                                  false,
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
      model_data.vertices.is_empty())
  {
    return nullptr;
  }

  GPUVertFormat format = {0};
  GPU_vertformat_attr_add(&format, "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  GPU_vertformat_attr_add(&format, "nor", gpu::VertAttrType::SFLOAT_32_32_32);
  GPU_vertformat_attr_add(&format, "texCoord", gpu::VertAttrType::SFLOAT_32_32);

  gpu::VertBuf *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, model_data.vertices.size());
  vbo->data<GHOST_XrControllerModelVertex>().copy_from(model_data.vertices);

  gpu::IndexBuf *ibo = nullptr;
  if (!(model_data.indices.is_empty()) && ((model_data.indices.size() % 3) == 0)) {
    GPUIndexBufBuilder ibo_builder;
    const uint prim_len = model_data.indices.size() / 3;
    GPU_indexbuf_init(&ibo_builder, GPU_PRIM_TRIS, prim_len, model_data.vertices.size());
    for (uint i = 0; i < prim_len; ++i) {
      const uint32_t *idx = &model_data.indices[i * 3];
      GPU_indexbuf_add_tri_verts(&ibo_builder, idx[0], idx[1], idx[2]);
    }
    ibo = GPU_indexbuf_build(&ibo_builder);
  }

  return GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, ibo, GPU_BATCH_OWNS_VBO | GPU_BATCH_OWNS_INDEX);
}

static ui::Layout &uiblock_prepare(ui::Block **block,
                                   const bContext *C,
                                   blender::ui::EmbossType emboss)
{
  const uiStyle *style = ui::style_get_dpi();
  const int viewfinder_width = style->widget.points * 50 * UI_SCALE_FAC;

  *block = ui::block_begin_xr(C, __func__, emboss);

  ui::block_flag_enable(*block, ui::BLOCK_LOOP | ui::BLOCK_KEEP_OPEN | ui::BLOCK_NO_WIN_CLIP);
  ui::block_theme_style_set(*block, ui::BLOCK_THEME_STYLE_POPUP); /* Can also use REGULAR here. */

  using namespace blender;
  return ui::block_layout(*block,
                          ui::LayoutDirection::Vertical,
                          ui::LayoutType::Panel,
                          0,
                          0,
                          viewfinder_width,
                          0,
                          0,
                          style);
}

static ui::Block *viewfinder_action_label_ui_block(const bContext *C,
                                                   const XrSessionSettings *settings)
{
  const char *active_action_prop = settings->viewfinder_active_mode == XR_VIEWFINDER_MODE_LIVE ?
                                       "viewfinder_active_action_live" :
                                       "viewfinder_active_action_playback";

  /* XR Session settings RNA pointer. */
  PointerRNA ptr = RNA_pointer_create_discrete(nullptr, RNA_XrSessionSettings, (void *)settings);
  //  PropertyRNA *prop = RNA_struct_find_property(&ptr, active_action_prop);

  ui::Block *block = nullptr;
  ui::Layout &layout = uiblock_prepare(&block, C, blender::ui::EmbossType::None);

  // TODO: Address the small menu down arrow that can be seen on the right side
  layout.prop(&ptr, active_action_prop, ui::ITEM_R_COMPACT | ui::ITEM_R_ICON_NEVER, "", ICON_NONE);

  ui::block_end_xr(C, block);

  return block;
}

static ui::Block *viewfinder_action_enum_ui_block(const bContext *C,
                                                  const XrSessionSettings *settings)
{
  /* XR Session settings RNA pointer. */
  PointerRNA ptr = RNA_pointer_create_discrete(nullptr, RNA_XrSessionSettings, (void *)settings);
  PropertyRNA *prop = RNA_struct_find_property(&ptr, "viewfinder_active_action_live");

  ui::Block *block = nullptr;
  ui::Layout &layout = uiblock_prepare(&block, C, blender::ui::EmbossType::Emboss);
  ui::Layout &row = layout.row(true);

  layout.scale_y_set(1.1f);

  if (settings->viewfinder_active_mode == XR_VIEWFINDER_MODE_LIVE) {
    /* Live mode, display each property enum separately for the DoF controls to be marked
     * as disabled when DoF is disabled. */
    layout.ui_units_x_set(8.0f); /* Width hack. */

    ui::Layout &sub1 = row.row(true);
    sub1.prop_enum(&ptr, prop, XR_VIEWFINDER_ACTION_LIVE_LENS, "", ICON_NONE);
    sub1.prop_enum(&ptr, prop, XR_VIEWFINDER_ACTION_LIVE_DOF, "", ICON_NONE);

    ui::Layout &sub2 = row.row(true);
    const Object *scene_cam = CTX_data_scene(C)->camera;
    const Camera *cam_data = id_cast<const Camera *>(scene_cam->data);
    sub2.enabled_set(cam_data->dof.flag & CAM_DOF_ENABLED);

    /* Show these controls greyed-out if DoF is disabled. */
    sub2.prop_enum(&ptr, prop, XR_VIEWFINDER_ACTION_LIVE_FOCUS, "", ICON_NONE);
    sub2.prop_enum(&ptr, prop, XR_VIEWFINDER_ACTION_LIVE_APERTURE, "", ICON_NONE);
  }
  else {
    /* Playback mode, directly draw the full enum prop. */
    layout.scale_x_set(15.0f); /* Width hack. */

    row.prop(&ptr,
             "viewfinder_active_action_playback",
             ui::ITEM_R_EXPAND | ui::ITEM_R_ICON_ONLY,
             "",
             ICON_NONE);
  }

  ui::block_end_xr(C, block);

  return block;
}

static ui::Block *viewfinder_settings_label_ui_block(const bContext *C,
                                                     const XrSessionSettings *settings)
{

  ui::Block *block = nullptr;
  ui::Layout &layout = uiblock_prepare(&block, C, blender::ui::EmbossType::Emboss);

  Scene *scene = CTX_data_scene(C);
  Object *cam_ob = scene->camera;
  const Camera *cam = id_cast<const Camera *>(cam_ob->data);

  PointerRNA scene_ptr = RNA_id_pointer_create(&scene->id);

  /* Note: unsafe, relies on the VR add-on to be loaded. */
  PropertyRNA *landmark_len_prop = RNA_struct_find_property(&scene_ptr, "vr_landmarks");
  PropertyRNA *landmark_idx_prop = RNA_struct_find_property(&scene_ptr, "vr_landmarks_selected");
  const int landmark_len = RNA_property_collection_length(&scene_ptr, landmark_len_prop);
  const int landmark_idx = RNA_property_int_get(&scene_ptr, landmark_idx_prop);

  std::string settings_label;
  switch (settings->viewfinder_active_mode) {
    case XR_VIEWFINDER_MODE_LIVE:
      settings_label = fmt::format("{}mm   DoF: {}   d: {:.1f}   f {:.1f}",
                                   cam->lens,
                                   (cam->dof.flag & CAM_DOF_ENABLED) ? "on" : "off",
                                   cam->dof.focus_distance,
                                   cam->dof.aperture_fstop);
      break;
    case XR_VIEWFINDER_MODE_PLAYBACK:
      settings_label = fmt::format("{} / {}", landmark_idx + 1, landmark_len);
      break;
    default:
      BLI_assert_unreachable();
      return nullptr;
  }

  layout.label(settings_label.c_str(), ICON_NONE);

  ui::block_end_xr(C, block);

  return block;
}

static ui::Block *viewfinder_mode_tabs_ui_block(const bContext *C,
                                                const XrSessionSettings *settings)
{
  ui::Block *block = ui::block_begin_xr(C, __func__, blender::ui::EmbossType::Emboss);
  ui::block_flag_enable(block, ui::BLOCK_LOOP | ui::BLOCK_KEEP_OPEN | ui::BLOCK_NO_WIN_CLIP);
  ui::block_theme_style_set(block, ui::BLOCK_THEME_STYLE_POPUP);

  const float tab_width = UI_UNIT_X * 10.5f;

  ui::Button *but = uiDefBut(block,
                             ui::ButtonType::Tab,
                             "Live Camera View",
                             0,
                             0,
                             tab_width,
                             UI_UNIT_Y,
                             nullptr,
                             0,
                             0,
                             "");
  button_func_pushed_state_set(but, [&settings](const ui::Button &) -> bool {
    return settings->viewfinder_active_mode == XR_VIEWFINDER_MODE_LIVE;
  });

  but = uiDefBut(block,
                 ui::ButtonType::Tab,
                 "Image Playback",
                 tab_width,
                 0,
                 tab_width,
                 UI_UNIT_Y,
                 nullptr,
                 0,
                 0,
                 "");
  button_func_pushed_state_set(but, [&settings](const ui::Button &) -> bool {
    return settings->viewfinder_active_mode == XR_VIEWFINDER_MODE_PLAYBACK;
  });

  ui::block_end_xr(C, block);

  return block;
}

static void wm_xr_controller_viewfinder_draw_ui_widgets(const bContext *C,
                                                        const XrSessionSettings *settings,
                                                        const rctf viewfinder_rect)
{
  /* Create a fake context to trick the UI drawing code in drawing in places it shouldn't be. */
  bContext *fake_C = CTX_copy(C);

  using BlockFuncPtr = decltype(&viewfinder_mode_tabs_ui_block);
  const auto draw_block = [&](BlockFuncPtr block_func, float x_off, float y_off) {
    GPU_matrix_push();
    GPU_matrix_translate_3f(x_off, y_off, 0.0f);
    GPU_matrix_scale_1f(0.02f);

    ui::Block *block = block_func(fake_C, settings);
    ui::block_draw_xr(fake_C, block); /* Stripped-down XR version of #UI_block_draw. */

    GPU_matrix_pop();
  };

  const float mode_tabs_x = viewfinder_rect.xmin - 0.15f;
  const float mode_tabs_y = viewfinder_rect.ymax + 0.45f;

  const float settings_label_x = settings->viewfinder_active_mode == XR_VIEWFINDER_MODE_LIVE ?
                                     viewfinder_rect.xmax - 3.40f :
                                     viewfinder_rect.xmax - 0.55f;
  const float settings_label_y = viewfinder_rect.ymax + 0.47f;

  const float action_label_x = viewfinder_rect.xmin - 0.1f;
  const float action_label_y = viewfinder_rect.ymin - 0.15f;

  const float action_enum_x = settings->viewfinder_active_mode == XR_VIEWFINDER_MODE_LIVE ?
                                  viewfinder_rect.xmax - 1.65f :
                                  viewfinder_rect.xmax - 1.25f;
  const float action_enum_y = viewfinder_rect.ymin - 0.15f;

  draw_block(viewfinder_mode_tabs_ui_block, mode_tabs_x, mode_tabs_y);
  draw_block(viewfinder_settings_label_ui_block, settings_label_x, settings_label_y);
  draw_block(viewfinder_action_label_ui_block, action_label_x, action_label_y);
  draw_block(viewfinder_action_enum_ui_block, action_enum_x, action_enum_y);
}

static void wm_xr_controller_viewfinder_draw_overlays(const rctf viewfinder_rect)
{
  /* Colors TODO: Dynamically get these from the current theme. */
  const float background_col[4] = {0.188f, 0.188f, 0.188f, 1.0f};
  const float outline_col[4] = {0.3f, 0.3f, 0.3f, 0.3f};

  rctf background_rect = viewfinder_rect;
  BLI_rctf_pad(&background_rect, 0.2f, 0.6f);
  BLI_rctf_translate(&background_rect, 0.0f, -0.1f);

  rctf outline_rect = viewfinder_rect;
  BLI_rctf_pad(&outline_rect, 0.08f, 0.08f);

  GPU_matrix_push();
  /* Workaround: regain precision on the rect side by a factor of 100. */
  GPU_matrix_scale_1f(0.01f);
  BLI_rctf_mul(&background_rect, 100);
  BLI_rctf_mul(&outline_rect, 100);

  /* Prevent other XR UI elements (like locomotion rays) from drawing through the viewfinder. */
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  ui::draw_roundbox_3fv_alpha(&background_rect, true, 16, background_col, 1.0f);
  GPU_depth_test(GPU_DEPTH_NONE);

  ui::draw_roundbox_3fv_alpha(&outline_rect, true, 12, outline_col, 0.2f);

  GPU_matrix_pop();
}

static void wm_xr_controller_viewfinder_draw_view_texture(const rctf viewfinder_rect)
{
  /* Obtain the Viewfinder view texture we computed in `wm_xr_draw_view()`. */
  blender::gpu::Texture *view_tex = GPU_offscreen_color_texture(g_viewfinder_offscreen);

  GPUVertFormat *view_text_format = immVertexFormat();
  uint view_tex_pos = GPU_vertformat_attr_add(
      view_text_format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);
  uint view_tex_coord = GPU_vertformat_attr_add(
      view_text_format, "texCoord", blender::gpu::VertAttrType::SFLOAT_32_32);

  GPU_depth_mask(false);
  GPU_blend(GPU_BLEND_ALPHA_PREMULT);

  immBindBuiltinProgram(GPU_SHADER_3D_IMAGE_COLOR);

  const float tex_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  immUniformColor4fv(tex_color);

  GPUSamplerExtendMode extend_mode = GPU_SAMPLER_EXTEND_MODE_REPEAT;
  immBindTextureSampler(
      "image", view_tex, {GPU_SAMPLER_FILTERING_LINEAR, extend_mode, extend_mode});

  immRectf_with_texco(view_tex_pos, view_tex_coord, viewfinder_rect, rctf{0.0f, 1.0f, 0.0f, 1.0f});

  immUnbindProgram();
}

static void wm_xr_controller_viewfinder_draw_view_flash(wmXrSessionState *state,
                                                        const XrSessionSettings *settings,
                                                        const rctf viewfinder_rect)
{
  /* Do not apply the flash effect if we're in playback mode. */
  if (settings->viewfinder_active_mode == XR_VIEWFINDER_MODE_PLAYBACK) {
    state->viewfinder_capture_flash = 0.0f;
    return;
  }

  /* Settings. */
  constexpr float flash_duration_sec = 0.4f;
  constexpr float full_flash_alpha = 0.3f;

  static double last_flash_time;
  if (state->viewfinder_capture_flash != 0.0f) {
    last_flash_time = BLI_time_now_seconds();
    state->viewfinder_capture_flash = 0.0f;
  }

  const float last_flash_delta = BLI_time_now_seconds() - last_flash_time;

  if (last_flash_delta < flash_duration_sec) {
    const float flash_progress = last_flash_delta / flash_duration_sec;
    const float flash_alpha = interpf(0.0f, full_flash_alpha, flash_progress);

    GPUVertFormat *flash_format = immVertexFormat();
    uint flash_pos = GPU_vertformat_attr_add(
        flash_format, "pos", blender::gpu::VertAttrType::SFLOAT_32_32);

    GPU_blend(GPU_BLEND_ALPHA);
    immBindBuiltinProgram(GPU_SHADER_3D_UNIFORM_COLOR);
    immUniformColor4f(1.0f, 1.0f, 1.0f, flash_alpha);
    immRectf(flash_pos,
             viewfinder_rect.xmin,
             viewfinder_rect.ymin,
             viewfinder_rect.xmax,
             viewfinder_rect.ymax);
    immUnbindProgram();
  }
}

static void wm_xr_controller_viewfinder_draw(const XrSessionSettings *settings,
                                             GHOST_IXrContext * /*xr_context*/,
                                             wmXrSessionState *state,
                                             const bContext *C)
{
  if (!settings->viewfinder_enable) {
    return;
  }

  const wmXrController *viewfinder_controller = get_viewfinder_controller(settings, state);
  if (!viewfinder_controller || !viewfinder_controller->grip_active) {
    return;
  }

  /* Fixed 16:9 aspect ratio for now. */
  const float viewfinder_height = settings->viewfinder_width * 9.0f / 16.0f;
  const float viewfinder_vertical_offset = 3.5f; /* Center of the viewfinder rectangle. */

  rctf viewfinder_rect = {0};
  BLI_rctf_resize(&viewfinder_rect, settings->viewfinder_width, viewfinder_height);

  /* Initial transform setup. */
  GPU_matrix_push();
  GPU_matrix_mul(viewfinder_controller->grip_mat);
  GPU_matrix_scale_1f(0.05f);
  GPU_matrix_translate_3f(0.0f, 0.0f, -viewfinder_vertical_offset);
  GPU_matrix_rotate_3f(-90.0f, 1.0f, 0.0f, 0.0f);

  /* Main background overlays. */
  wm_xr_controller_viewfinder_draw_overlays(viewfinder_rect);

  /* Viewfinder View texture and flash. */
  wm_xr_controller_viewfinder_draw_view_texture(viewfinder_rect);
  wm_xr_controller_viewfinder_draw_view_flash(state, settings, viewfinder_rect);

  /* UI Widgets. */
  wm_xr_controller_viewfinder_draw_ui_widgets(C, settings, viewfinder_rect);

  GPU_matrix_pop();
}

static void wm_xr_controller_model_textures_create(GHOST_IXrContext *xr_context,
                                                   const char *subaction_path,
                                                   wmXrController &controller)
{
  GHOST_XrControllerModelData model_data;

  if (!GHOST_XrGetControllerModelData(xr_context, subaction_path, &model_data) ||
      model_data.textures.is_empty())
  {
    return;
  }

  for (const GHOST_XrControllerModelTextureData &texture : model_data.textures) {
    if (texture.empty()) {
      continue;
    }

    /* Decode raw glTF texture image data using ImBuf. */
    ImBuf *ibuf = IMB_load_image_from_memory(
        texture.data(), texture.size(), IB_byte_data, "xr_controller_tex_image");

    if (!ibuf) {
      continue;
    }

    /* Create GPU texture. */
    gpu::Texture *model_texture = GPU_texture_create_2d("xr_controller_tex",
                                                        ibuf->x,
                                                        ibuf->y,
                                                        1,
                                                        gpu::TextureFormat::SRGBA_8_8_8_8,
                                                        GPU_TEXTURE_USAGE_SHADER_READ,
                                                        nullptr);
    GPU_texture_update(model_texture, GPU_DATA_UBYTE, ibuf->byte_buffer.data);
    controller.model_textures.append(model_texture);

    IMB_freeImBuf(ibuf);
  }
}


static void wm_xr_controller_model_draw(const XrSessionSettings *settings,
                                        GHOST_IXrContext *xr_context,
                                        wmXrSessionState *state,
                                        const bContext *C)
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

  GPU_blend(GPU_BLEND_ALPHA);

  for (wmXrController &controller : state->controllers) {
    if (!controller.grip_active) {
      continue;
    }

    gpu::Batch *model = controller.model;
    if (!model) {
      model = controller.model = wm_xr_controller_model_batch_create(xr_context,
                                                                     controller.subaction_path);
      wm_xr_controller_model_textures_create(xr_context, controller.subaction_path, controller);
    }

    if (model &&
        GHOST_XrGetControllerModelData(xr_context, controller.subaction_path, &model_data) &&
        !model_data.components.is_empty())
    {
      GPU_matrix_push();
      GPU_matrix_mul(controller.model_mat);

      for (const GHOST_XrControllerModelComponent &component: model_data.components) {
        /* Check if this component has a texture. */
        gpu::Texture *texture = nullptr;
        if (component.texture_index >= 0 &&
            component.texture_index < controller.model_textures.size())
        {
          texture = controller.model_textures[component.texture_index];
        }

        GPU_matrix_push();
        GPU_matrix_mul(component.transform.ptr());

        if (texture) {
          /* Use textured model. */
          GPU_batch_program_set_builtin(model, GPU_SHADER_3D_IMAGE);
          int binding = GPU_shader_get_sampler_binding(model->shader, "image");
          GPU_texture_bind(texture, binding);
          GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
        }
        else {
          /* Fallback to transparent model with solid color. */
          GPU_batch_program_set_builtin(model, GPU_SHADER_3D_UNIFORM_COLOR);
          GPU_batch_uniform_4fv(model, "color", color);
          GPU_depth_test(GPU_DEPTH_NONE);
        }

        GPU_batch_draw_range(model,
                             model->elem ? component.index_offset : component.vertex_offset,
                             model->elem ? component.index_count : component.vertex_count);

        if (texture) {
          GPU_texture_unbind(texture);
        }

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

  wm_xr_controller_viewfinder_draw(settings, xr_context, state, C);
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

void wm_xr_draw_controllers(const bContext * /*C*/, ARegion * /*region*/, void *customdata)
{
  wmXrData *xr = static_cast<wmXrData *>(customdata);
  const XrSessionSettings *settings = &xr->session_settings;
  GHOST_IXrContext *xr_context = xr->runtime->ghost_context;
  wmXrSessionState *state = &xr->runtime->session_state;

  wm_xr_controller_model_draw(settings, xr_context, state, evil_main_C);
  wm_xr_controller_aim_draw(settings, state);
}

}  // namespace blender
