/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "overlay_base.hh"
#include "overlay_empty.hh"

namespace blender {
struct CameraBGImage;
}

namespace blender::draw::overlay {
struct CameraInstanceData : public ExtraInstanceData {
 public:
  float &volume_start = color_[2];
  float &volume_end = color_[3];
  float &depth = color_[3];
  float &focus = color_[3];
  float4x4 &matrix = object_to_world;
  float &dist_color_id = matrix[0][3];
  float &corner_x = matrix[0][3];
  float &corner_y = matrix[1][3];
  float &center_x = matrix[2][3];
  float &clip_start = matrix[2][3];
  float &mist_start = matrix[2][3];
  float &center_y = matrix[3][3];
  float &clip_end = matrix[3][3];
  float &mist_end = matrix[3][3];

  CameraInstanceData(const CameraInstanceData &data)
      : CameraInstanceData(data.object_to_world, data.color_)
  {
  }

  CameraInstanceData(const float4x4 &p_matrix, const float4 &color)
      : ExtraInstanceData(p_matrix, color, 1.0f) {};
};

/**
 * Camera object display (including stereoscopy).
 * Also camera reconstruction bundles.
 * Also camera reference images (background).
 */
/* TODO(fclem): Split into multiple overlay classes. */
class Cameras : Overlay {
  using CameraInstanceBuf = ShapeInstanceBuf<ExtraInstanceData>;

 private:
  PassSimple ps_ = {"Cameras"};

  /* Camera background images with "Depth" switched to "Back".
   * Shown in camera view behind all objects. */
  PassMain background_ps_ = {"background_ps_"};
  /* Camera background images with "Depth" switched to "Front".
   * Shown in camera view in front of all objects. */
  PassMain foreground_ps_ = {"foreground_ps_"};

  /* Same as `background_ps_` with "View as Render" checked. */
  PassMain background_scene_ps_ = {"background_scene_ps_"};
  /* Same as `foreground_ps_` with "View as Render" checked. */
  PassMain foreground_scene_ps_ = {"foreground_scene_ps_"};

  struct CallBuffers {
    const SelectionType selection_type_;
    CameraInstanceBuf distances_buf = {selection_type_, "camera_distances_buf"};
    CameraInstanceBuf frame_buf = {selection_type_, "camera_frame_buf"};
    CameraInstanceBuf tria_buf = {selection_type_, "camera_tria_buf"};
    CameraInstanceBuf tria_wire_buf = {selection_type_, "camera_tria_wire_buf"};
    CameraInstanceBuf volume_buf = {selection_type_, "camera_volume_buf"};
    CameraInstanceBuf volume_wire_buf = {selection_type_, "camera_volume_wire_buf"};
    CameraInstanceBuf sphere_solid_buf = {selection_type_, "camera_sphere_solid_buf"};
    LinePrimitiveBuf stereo_connect_lines = {selection_type_, "camera_dashed_lines_buf"};
    LinePrimitiveBuf tracking_path = {selection_type_, "camera_tracking_path_buf"};
    Empties::CallBuffers empties{selection_type_};
  } call_buffers_;

  bool images_enabled_ = false;
  bool extras_enabled_ = false;
  bool motion_tracking_enabled_ = false;

  View::OffsetData offset_data_;
  float4x4 depth_bias_winmat_;

 public:
  Cameras(const SelectionType selection_type) : call_buffers_{selection_type} {};

  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void end_sync(Resources &res, const State &state) final;

  void pre_draw(Manager &manager, View &view) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;

  void draw_scene_background_images(gpu::FrameBuffer *framebuffer, Manager &manager, View &view);

  void draw_background_images(Framebuffer &framebuffer, Manager &manager, View &view);

  void draw_in_front(Framebuffer &framebuffer, Manager &manager, View &view);

 private:
  void object_sync_extras(const ObjectRef &ob_ref,
                          select::ID select_id,
                          const State &state,
                          Resources &res);

  void object_sync_motion_paths(const ObjectRef &ob_ref, Resources &res, const State &state);

  void object_sync_images(const ObjectRef &ob_ref,
                          select::ID select_id,
                          Manager &manager,
                          const State &state,
                          Resources &res);

  gpu::Texture *image_camera_background_texture_get(const CameraBGImage *bgpic,
                                                    const State &state,
                                                    Resources &res,
                                                    float &r_aspect,
                                                    bool &r_use_alpha_premult,
                                                    bool &r_use_view_transform);

  /**
   * Draw the stereo 3d support elements (cameras, plane, volume).
   * They are only visible when not looking through the camera:
   */
  void sync_stereoscopy_extra(const Main &bmain,
                              const CameraInstanceData &instdata,
                              const select::ID cam_select_id,
                              const Scene *scene,
                              const View3D *v3d,
                              Resources &res,
                              Object *ob);
};

}  // namespace blender::draw::overlay
