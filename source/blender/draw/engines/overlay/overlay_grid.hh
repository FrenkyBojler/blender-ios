/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "DEG_depsgraph_query.hh"

#include "DNA_camera_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "ED_image.hh"
#include "ED_view3d.hh"
#include "GPU_texture.hh"

#include "draw_shader_shared.hh"
#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Grid draw rework; WIP.
 */
class GridRework : Overlay {
 private:
  UniformBuffer<OVERLAY_GridReworkData> grid_ubo_;
  StorageVectorBuffer<float4> tile_pos_buf_;
  PassSimple grid_ps_ = {"grid_ps_"};

  /* General parameters. */
  bool is_3d_grid_ = false;
  uint num_lines_per_level_;

  /* Draw information. */
  float2 grid_poi_origin_ = float2(0.0f);
  float2 grid_poi_ = float2(0.0f);
  float grid_level_;
  int lines_count_ = 0;
  int grid_flag_ = 0;

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    is_3d_grid_ = state.is_space_v3d();
    enabled_ = !state.is_space_node() && init(state);

    if (!enabled_) {
      grid_ps_.init();
      return;
    }

    gpu::Texture **depth_tx = state.xray_enabled ? &res.xray_depth_tx : &res.depth_tx;
    gpu::Texture **depth_infront_tx = state.use_in_front ? &res.depth_target_in_front_tx :
                                                           &res.dummy_depth_tx;

    grid_ps_.init();
    grid_ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    grid_ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
    grid_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_WRITE_DEPTH | DRW_STATE_BLEND_ALPHA |
                       DRW_STATE_DEPTH_LESS_EQUAL);

    /* Draw a quad behind the grid, specifically in the 2D/uv image editor. This is retained
     * from the 5.0 grid. */
    if (state.is_space_image()) {
      auto &sub = grid_ps_.sub("grid_background");
      sub.shader_set(res.shaders->grid_background.get());
      const float4 color_back = math::interpolate(
          res.theme.colors.background, res.theme.colors.grid, 0.5);
      sub.push_constant("ucolor", color_back);
      sub.push_constant("tile_scale", float3(grid_ubo_.size));
      sub.bind_texture("depth_buffer", depth_tx);
      sub.draw(res.shapes.quad_solid.get());
    }

    /* Grid and axis line draws. */
    {
      auto &sub = grid_ps_.sub("grid");

      /* Vertex count is 2 (x/y direction) * 2 (per line) * levels x N, plus
        * one optional line for the Z-axis, if this is perpendicular to the XY plane. */
      const bool incl_axis_z = (grid_flag_ & PLANE_XY) && (grid_flag_ & SHOW_AXIS_Z);
      const uint verts_count = 4 * OVERLAY_GRID_STEPS_DRAW * lines_count_ + 2 * uint(incl_axis_z);

      sub.shader_set(res.shaders->gridrework.get());
      sub.bind_ubo("grid_buf", &grid_ubo_);
      /* TODO(not_mark): remove */
      sub.bind_texture("depth_tx", depth_tx, GPUSamplerState::default_sampler());
      sub.bind_texture("depth_infront_tx", depth_infront_tx, GPUSamplerState::default_sampler());
      sub.push_constant("grid_level", &grid_level_);
      sub.push_constant("grid_poi", &grid_poi_);
      sub.push_constant("grid_flag", &grid_flag_);
      sub.push_constant("num_lines", &lines_count_);
      sub.draw_procedural(GPUPrimType::GPU_PRIM_LINES, -1, verts_count, 0);
    }

    /* Draw an outline around the grid, specifically in the 2D/UV image editor. This is retained
     * from the 5.0 grid. */
    if (state.is_space_image()) {
      float4 theme_color;
      UI_GetThemeColorShade4fv(TH_BACK, 60, theme_color);
      srgb_to_linearrgb_v4(theme_color, theme_color);

      /* Add wire border. */
      auto &sub = grid_ps_.sub("wire_border");
      sub.shader_set(res.shaders->grid_image.get());
      sub.push_constant("ucolor", theme_color);
      tile_pos_buf_.clear();
      for (const int x : IndexRange(grid_ubo_.size[0])) {
        for (const int y : IndexRange(grid_ubo_.size[1])) {
          tile_pos_buf_.append(float4(x, y, 0.0f, 0.0f));
        }
      }
      tile_pos_buf_.push_update();
      sub.bind_ssbo("tile_pos_buf", &tile_pos_buf_);
      sub.draw(res.shapes.quad_wire.get(), tile_pos_buf_.size());
    }
  }

  void draw_color_only(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    grid_ubo_.push_update();
    GPU_framebuffer_bind(framebuffer);
    manager.submit(grid_ps_, view);
  }

 private:
  bool init(const State &state)
  {
    /* Initialize config flags to default value. */
    grid_flag_ = 0;

    /* This suffices for most cases, and in others we fade to hide it. */
    num_lines_per_level_ = 301; /* TODO(not_mark): variable line count for orth/persp/uv/image */
    /* TODO(not_mark): remove */
    grid_ubo_.num_lines_per_level = num_lines_per_level_;

    return is_3d_grid_ ? init_3d(state) : init_2d(state);
  }

  bool init_2d(const State &state)
  {
    if (state.hide_overlays) {
      return false;
    }

    const View2D *v2d = &state.region->v2d;
    SpaceImage *sima = (SpaceImage *)state.space_data;

    /* Query different options from overlay/spaceimage state. Only UV edit has
     * overlay options for now. */
    const bool is_uv_edit = sima->mode == SI_MODE_UV;
    const bool background_enabled = is_uv_edit ? (sima->overlay.flag &
                                                  SI_OVERLAY_SHOW_GRID_BACKGROUND) != 0 :
                                                 true;
    const bool draw_grid = is_uv_edit || !ED_space_image_has_buffer(sima);

    /* Process grid flags. */
    if (background_enabled) {
      grid_flag_ = GRID_BACK | PLANE_IMAGE;
      if (sima->flag & SI_GRID_OVER_IMAGE) {
        grid_flag_ = PLANE_IMAGE;
      }
    }
    if (background_enabled && draw_grid) {
      grid_flag_ |= SHOW_GRID;
      if (is_uv_edit && sima->grid_shape_source != SI_GRID_SHAPE_DYNAMIC) {
        grid_flag_ |= CUSTOM_GRID;
      }
    }

    /* Query grid step/level scalings; these can differ per axis. */
    std::array<float, SI_GRID_STEPS_LEN> steps_x, steps_y;
    ED_space_image_grid_steps(sima, steps_x.data(), steps_y.data(), SI_GRID_STEPS_LEN);
    for (int i = 0; i < SI_GRID_STEPS_LEN; ++i) {
      grid_ubo_.level_scales[i].x = steps_x[i] * 2.0f;
      grid_ubo_.level_scales[i].y = steps_y[i] * 2.0f;
    }

    /* Determine camera offset to center of v2d. */
    grid_ubo_.distance = 1.0f;
    grid_poi_ = float2(v2d->cur.xmax + v2d->cur.xmin, v2d->cur.ymax + v2d->cur.ymin) - 1.0f;

    /* Query grid image zoom level. Then find the lowest relevant grid level + fractional. */
    float dist = ED_space_image_zoom_level(v2d, SI_GRID_STEPS_LEN) * 4.0f;
    for (int i = 0; i < OVERLAY_GRID_STEPS_LEN + 1; i++) {
      float prev = (i > 0) ?
                       std::min(grid_ubo_.level_scales[i - 1].x, grid_ubo_.level_scales[i - 1].y) :
                       0.0f;
      float curr = (i < OVERLAY_GRID_STEPS_LEN) ?
                       std::min(grid_ubo_.level_scales[i].x, grid_ubo_.level_scales[i].y) :
                       std::numeric_limits<float>::infinity();

      if (curr >= dist || i == OVERLAY_GRID_STEPS_LEN) {
        grid_level_ = static_cast<float>(i) + safe_divide(dist - prev, curr - prev);
        break;
      }
    }

    /* TODO (not_mark): detail what's being stored here. Grid res basically. */
    grid_ubo_.size = float4(1.0f);
    if (is_uv_edit) {
      grid_ubo_.size[0] = float(sima->tile_grid_shape[0]);
      grid_ubo_.size[1] = float(sima->tile_grid_shape[1]);
    }

    lines_count_ = (grid_flag_ & SHOW_GRID) ? num_lines_per_level_ : 1;

    return true;
  }

  bool init_3d(const State &state)
  {
    /* Query different options from overlay state */
    const bool show_axis_x = (state.v3d_gridflag & V3D_SHOW_X) != 0;
    const bool show_axis_y = (state.v3d_gridflag & V3D_SHOW_Y) != 0;
    const bool show_axis_z = (state.v3d_gridflag & V3D_SHOW_Z) != 0;
    const bool show_persp = (state.v3d_gridflag & V3D_SHOW_FLOOR) != 0;
    const bool show_ortho = (state.v3d_gridflag & V3D_SHOW_ORTHO_GRID) != 0;
    const bool show_any = show_axis_x || show_axis_y || show_axis_z || show_persp || show_ortho;

    if (state.hide_overlays || !show_any) {
      return false;
    }

    const View3D *v3d = state.v3d;
    const RegionView3D *rv3d = state.rv3d;

    /* Set `grid_flag_` dependent on view configuration. */
    if (rv3d->is_persp || rv3d->view == RV3D_VIEW_USER) {
      /* Perspective; set selected axes and floor bits. */
      grid_flag_ |= (show_axis_x ? PLANE_XY | SHOW_AXIS_X : OVERLAY_GridBits(0));
      grid_flag_ |= (show_axis_y ? PLANE_XY | SHOW_AXIS_Y : OVERLAY_GridBits(0));
      grid_flag_ |= (show_axis_z ? PLANE_XY | SHOW_AXIS_Z : OVERLAY_GridBits(0));
      grid_flag_ |= (show_persp ? PLANE_XY | SHOW_GRID : OVERLAY_GridBits(0));
    }
    else {
      /* Orthographic; set selected axes and plane bits dependent on the specific view
       * (top, right, left, etc.) that is selected. */
      int grid_x_flag = show_axis_x ? SHOW_AXIS_X : OVERLAY_GridBits(0);
      int grid_y_flag = show_axis_y ? SHOW_AXIS_Y : OVERLAY_GridBits(0);
      int grid_z_flag = show_axis_z ? SHOW_AXIS_Z : OVERLAY_GridBits(0);
      if (ELEM(rv3d->view, RV3D_VIEW_RIGHT, RV3D_VIEW_LEFT)) {
        grid_flag_ = PLANE_YZ | grid_y_flag | grid_z_flag;
      }
      else if (ELEM(rv3d->view, RV3D_VIEW_TOP, RV3D_VIEW_BOTTOM)) {
        grid_flag_ = PLANE_XY | grid_x_flag | grid_y_flag;
      }
      else if (ELEM(rv3d->view, RV3D_VIEW_FRONT, RV3D_VIEW_BACK)) {
        grid_flag_ = PLANE_XZ | grid_x_flag | grid_z_flag;
      }
      grid_flag_ |= (show_ortho ? GRID_BACK | SHOW_GRID : OVERLAY_GridBits(0));
    }

    /* Query far clip distance dependent on camera/viewport */
    if (rv3d->persp == RV3D_CAMOB && v3d->camera && v3d->camera->type == OB_CAMERA) {
      Object *camera_object = DEG_get_evaluated(state.depsgraph, v3d->camera);
      grid_ubo_.distance = ((Camera *)(camera_object->data))->clip_end;
      grid_flag_ |= GRID_CAMERA;
    }
    else {
      grid_ubo_.distance = v3d->clip_end;
    }

    /* Query grid scales from unit/scaling; this range suffices for user-visible levels. */
    Array<float, SI_GRID_STEPS_LEN> steps(SI_GRID_STEPS_LEN);
    ED_view3d_grid_steps(state.scene, v3d, rv3d, steps.data());
    for (int i = 0; i < SI_GRID_STEPS_LEN; ++i) {
      grid_ubo_.level_scales[i].x = grid_ubo_.level_scales[i].y = steps[i];
    }

    /* Camera parameters. */
    float3 drw_view_position = rv3d->viewinv[3], drw_view_forward = rv3d->viewinv[2];

    /* Compute distance to a relevant floor point-of-interest from the camera. The grid translates
     * with this point and is only drawn around it. */
    float dist;
    if (rv3d->is_persp) {
      /* Scale depends on distance to a point on the floor plane; we interpolate between the
       * point viewed by the camera and the point directly below it, dependent on azimuth. */
      dist = interpolate(abs(drw_view_position.z / drw_view_forward.z),
                         abs(drw_view_position.z),
                         1.0f - abs(drw_view_forward.z));
    }
    else {
      /* Scale is simply specified by orthographic view. */
      dist = rv3d->dist;
    }

    /* Extract 2D grid offset for moving grid "with the camera" on the floor plane. */
    float3 camera_poi = drw_view_position - dist * drw_view_forward;
    if (ELEM(rv3d->view, RV3D_VIEW_RIGHT, RV3D_VIEW_LEFT)) {
      grid_poi_ = camera_poi.yz();
    }
    else if (ELEM(rv3d->view, RV3D_VIEW_TOP, RV3D_VIEW_BOTTOM)) {
      grid_poi_ = camera_poi.xy();
    }
    else if (ELEM(rv3d->view, RV3D_VIEW_FRONT, RV3D_VIEW_BACK)) {
      grid_poi_ = float2(camera_poi.x, camera_poi.z);
    }
    else { /* Perspective view, Image/UV view. */
      grid_poi_ = camera_poi.xy();
    }

    /* Find the lowest relevant grid level + fractional, dependent on camera distance. We
     * fake a order of magnitude extra level, as in orthographic cameras the maximum zoom
     * barely exceeds the largest specified grid scale in unit systems. */
    /* TODO(not_mark): half of this loop is unreachable. Fix. */
    for (int i = 0; i < OVERLAY_GRID_STEPS_LEN - 1; i++) {
      float curr = std::min(grid_ubo_.level_scales[i].x, grid_ubo_.level_scales[i].y);
      float next = (i < OVERLAY_GRID_STEPS_LEN - 1) ?
                       std::min(grid_ubo_.level_scales[i + 1].x, grid_ubo_.level_scales[i + 1].y) :
                       curr * 10.0f;
      if (next >= dist || i == OVERLAY_GRID_STEPS_LEN - 1) {
        grid_level_ = static_cast<float>(i) + safe_divide(dist - curr, next - curr);
        break;
      }
    }

    lines_count_ = (grid_flag_ & SHOW_GRID) ? num_lines_per_level_ : 1;

    return true;
  }
};

/**
 * Draw 2D or 3D grid as well at global X, Y and Z axes.
 */
class Grid : Overlay {
 private:
  UniformBuffer<OVERLAY_GridData> data_;
  StorageVectorBuffer<float4> tile_pos_buf_;

  PassSimple grid_ps_ = {"grid_ps_"};

  bool show_axis_z_ = false;
  bool is_xr_ = false;
  bool is_3d_grid_ = false;
  /* Copy of v3d->dist. */
  float v3d_clip_end_ = 0.0f;

  float3 grid_axes_ = float3(0.0f);
  float3 zplane_axes_ = float3(0.0f);
  int grid_flag_ = 0;
  int zneg_flag_ = 0;
  int zpos_flag_ = 0;

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    is_3d_grid_ = state.is_space_v3d();

    enabled_ = !state.is_space_node() && init(state);
    if (!enabled_) {
      grid_ps_.init();
      return;
    }

    gpu::Texture **depth_tx = state.xray_enabled ? &res.xray_depth_tx : &res.depth_tx;
    gpu::Texture **depth_infront_tx = state.use_in_front ? &res.depth_target_in_front_tx :
                                                           &res.dummy_depth_tx;

    grid_ps_.init();
    grid_ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    grid_ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
    grid_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA);

    /* NOTE(not_mark): this does a fullscreen color draw, with the same color
     * as the theme.uv.grid settings. Seems... redundant? Also inconsistent with
     * the 3D viewport. */
    if (state.is_space_image()) {
      /* Add quad background. */
      auto &sub = grid_ps_.sub("grid_background");
      sub.shader_set(res.shaders->grid_background.get());
      const float4 color_back = math::interpolate(
          res.theme.colors.background, res.theme.colors.grid, 0.5);
      sub.push_constant("ucolor", color_back);
      sub.push_constant("tile_scale", float3(data_.size));
      sub.bind_texture("depth_buffer", depth_tx);
      sub.draw(res.shapes.quad_solid.get());
    }

    /* NOTE(not_mark):
     * - Draws axis lines, zneg and zpos, 3d only
     * - Draws grid lines, 2d or 3d
     * - Draws axis lines, zneg and zpos, 3d only
     */
    {
      auto &sub = grid_ps_.sub("grid");
      sub.shader_set(res.shaders->grid.get());
      sub.bind_ubo("grid_buf", &data_);
      sub.bind_texture("depth_tx", depth_tx, GPUSamplerState::default_sampler());
      sub.bind_texture("depth_infront_tx", depth_infront_tx, GPUSamplerState::default_sampler());
      if (zneg_flag_ & SHOW_AXIS_Z) {
        sub.push_constant("grid_flag", &zneg_flag_);
        sub.push_constant("plane_axes", &zplane_axes_);
        sub.draw(res.shapes.grid.get());
      }
      if (grid_flag_) {
        sub.push_constant("grid_flag", &grid_flag_);
        sub.push_constant("plane_axes", &grid_axes_);
        sub.draw(res.shapes.grid.get());
      }
      if (zpos_flag_ & SHOW_AXIS_Z) {
        sub.push_constant("grid_flag", &zpos_flag_);
        sub.push_constant("plane_axes", &zplane_axes_);
        sub.draw(res.shapes.grid.get());
      }
    }

    /* NOTE(not_mark): Draws grid outline wire border. Bit more clever than just that though. */
    if (state.is_space_image()) {
      float4 theme_color;
      ui::theme::get_color_shade_4fv(TH_BACK, 60, theme_color);
      srgb_to_linearrgb_v4(theme_color, theme_color);

      /* Add wire border. */
      auto &sub = grid_ps_.sub("wire_border");
      sub.shader_set(res.shaders->grid_image.get());
      sub.push_constant("ucolor", theme_color);
      tile_pos_buf_.clear();
      for (const int x : IndexRange(data_.size[0])) {
        for (const int y : IndexRange(data_.size[1])) {
          tile_pos_buf_.append(float4(x, y, 0.0f, 0.0f));
        }
      }
      tile_pos_buf_.push_update();
      sub.bind_ssbo("tile_pos_buf", &tile_pos_buf_);
      sub.draw(res.shapes.quad_wire.get(), tile_pos_buf_.size());
    }
  }

  void draw_color_only(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    sync_view(view);
    data_.push_update();

    GPU_framebuffer_bind(framebuffer);
    manager.submit(grid_ps_, view);
  }

 private:
  bool init(const State &state)
  {
    data_.line_size = max_ff(0.0f, U.pixelsize - 1.0f) * 0.5f;
    /* Default, nothing is drawn. */
    grid_flag_ = zneg_flag_ = zpos_flag_ = 0;
    show_axis_z_ = false;

    return (is_3d_grid_) ? init_3d(state) : init_2d(state);
  }

  void copy_steps_to_data(Span<float> grid_steps_x, Span<float> grid_steps_y)
  {
    /* Convert to UBO alignment. */
    for (const int i : IndexRange(SI_GRID_STEPS_LEN)) {
      data_.steps[i][0] = grid_steps_x[i];
      data_.steps[i][1] = grid_steps_y[i];
    }
  }

  bool init_2d(const State &state)
  {
    if (state.hide_overlays) {
      return false;
    }
    SpaceImage *sima = (SpaceImage *)state.space_data;
    const View2D *v2d = &state.region->v2d;
    std::array<float, SI_GRID_STEPS_LEN> grid_steps_x = {
        0.001f, 0.01f, 0.1f, 1.0f, 10.0f, 100.0f, 1000.0f, 10000.0f};
    std::array<float, SI_GRID_STEPS_LEN> grid_steps_y = {0.0f};

    /* Only UV Edit mode has the various Overlay options for now. */
    const bool is_uv_edit = sima->mode == SI_MODE_UV;

    const bool background_enabled = is_uv_edit ? (!state.hide_overlays &&
                                                  (sima->overlay.flag &
                                                   SI_OVERLAY_SHOW_GRID_BACKGROUND) != 0) :
                                                 true;
    if (background_enabled) {
      grid_flag_ = GRID_BACK | PLANE_IMAGE;
      if (sima->flag & SI_GRID_OVER_IMAGE) {
        grid_flag_ = PLANE_IMAGE;
      }
    }

    const bool draw_grid = is_uv_edit || !ED_space_image_has_buffer(sima);
    if (background_enabled && draw_grid) {
      grid_flag_ |= SHOW_GRID;
      if (is_uv_edit) {
        if (sima->grid_shape_source != SI_GRID_SHAPE_DYNAMIC) {
          grid_flag_ |= CUSTOM_GRID;
        }
      }
    }

    data_.distance = 1.0f;
    data_.size = float4(1.0f);
    if (is_uv_edit) {
      data_.size[0] = float(sima->tile_grid_shape[0]);
      data_.size[1] = float(sima->tile_grid_shape[1]);
    }

    data_.zoom_factor = ED_space_image_zoom_level(v2d, SI_GRID_STEPS_LEN);
    ED_space_image_grid_steps(sima, grid_steps_x.data(), grid_steps_y.data(), SI_GRID_STEPS_LEN);
    copy_steps_to_data(grid_steps_x, grid_steps_y);
    return true;
  }

  bool init_3d(const State &state)
  {
    const View3D *v3d = state.v3d;
    const RegionView3D *rv3d = state.rv3d;

    const bool show_axis_x = (state.v3d_gridflag & V3D_SHOW_X) != 0;
    const bool show_axis_y = (state.v3d_gridflag & V3D_SHOW_Y) != 0;
    const bool show_axis_z = (state.v3d_gridflag & V3D_SHOW_Z) != 0;
    const bool show_persp = (state.v3d_gridflag & V3D_SHOW_FLOOR) != 0;
    const bool show_ortho_grid = (state.v3d_gridflag & V3D_SHOW_ORTHO_GRID) != 0;
    const bool show_any = show_axis_x || show_axis_y || show_axis_z || show_persp ||
                          show_ortho_grid;

    if (state.hide_overlays || !show_any) {
      return false;
    }

    std::array<float, SI_GRID_STEPS_LEN> grid_steps = {
        0.001f, 0.01f, 0.1f, 1.0f, 10.0f, 100.0f, 1000.0f, 10000.0f};

    /* If perspective view or non-axis aligned view. */
    if (rv3d->is_persp || rv3d->view == RV3D_VIEW_USER) {
      if (show_axis_x) {
        grid_flag_ |= PLANE_XY | SHOW_AXIS_X;
      }
      if (show_axis_y) {
        grid_flag_ |= PLANE_XY | SHOW_AXIS_Y;
      }
      if (show_persp) {
        grid_flag_ |= PLANE_XY | SHOW_GRID;
      }
    }
    else {
      if (ELEM(rv3d->view, RV3D_VIEW_RIGHT, RV3D_VIEW_LEFT)) {
        grid_flag_ = PLANE_YZ | (show_axis_y ? SHOW_AXIS_Y : OVERLAY_GridBits(0)) |
                     (show_axis_z ? SHOW_AXIS_Z : OVERLAY_GridBits(0));
      }
      else if (ELEM(rv3d->view, RV3D_VIEW_TOP, RV3D_VIEW_BOTTOM)) {
        grid_flag_ = PLANE_XY | (show_axis_x ? SHOW_AXIS_X : OVERLAY_GridBits(0)) |
                     (show_axis_y ? SHOW_AXIS_Y : OVERLAY_GridBits(0));
      }
      else if (ELEM(rv3d->view, RV3D_VIEW_FRONT, RV3D_VIEW_BACK)) {
        grid_flag_ = PLANE_XZ | (show_axis_x ? SHOW_AXIS_X : OVERLAY_GridBits(0)) |
                     (show_axis_z ? SHOW_AXIS_Z : OVERLAY_GridBits(0));
      }
      if (show_ortho_grid) {
        grid_flag_ |= SHOW_GRID | GRID_BACK;
      }
    }

    grid_axes_[0] = float((grid_flag_ & (PLANE_XZ | PLANE_XY)) != 0);
    grid_axes_[1] = float((grid_flag_ & (PLANE_YZ | PLANE_XY)) != 0);
    grid_axes_[2] = float((grid_flag_ & (PLANE_YZ | PLANE_XZ)) != 0);

    /* Z axis if needed */
    if (((rv3d->view == RV3D_VIEW_USER) || (rv3d->persp != RV3D_ORTHO)) && show_axis_z) {
      zpos_flag_ = zneg_flag_ = SHOW_AXIS_Z;
    }
    else {
      zneg_flag_ = zpos_flag_ = DRAW_AXIS_ZNEG | DRAW_AXIS_Z;
    }

    if (rv3d->persp == RV3D_CAMOB && v3d->camera && v3d->camera->type == OB_CAMERA) {
      Object *camera_object = DEG_get_evaluated(state.depsgraph, v3d->camera);
      v3d_clip_end_ = ((Camera *)(camera_object->data))->clip_end;
      grid_flag_ |= GRID_CAMERA;
      zneg_flag_ |= GRID_CAMERA;
      zpos_flag_ |= GRID_CAMERA;
    }
    else {
      v3d_clip_end_ = v3d->clip_end;
    }

    ED_view3d_grid_steps(state.scene, v3d, rv3d, grid_steps.data());

    is_xr_ = (v3d->flag & (V3D_XR_SESSION_SURFACE | V3D_XR_SESSION_MIRROR)) != 0;

    copy_steps_to_data(grid_steps, grid_steps);
    return true;
  }

  /* Update data that depends on the view. */
  void sync_view(const View &view)
  {
    if (!is_3d_grid_) {
      return;
    }

    if (zpos_flag_ & SHOW_AXIS_Z) {
      float3 backward = -view.forward();
      float3 position = view.location();

      /* z axis : chose the most facing plane */
      if (fabsf(backward.x) < fabsf(backward.y)) {
        zpos_flag_ |= PLANE_XZ;
      }
      else {
        zpos_flag_ |= PLANE_YZ;
      }
      zneg_flag_ = zpos_flag_;

      /* Perspective: If camera is below floor plane, we switch clipping.
       * Orthographic: If eye vector is looking up, we switch clipping. */
      if ((view.is_persp() && (position.z > 0.0f)) || (!view.is_persp() && (backward.z < 0.0f))) {
        zpos_flag_ |= DRAW_AXIS_Z;
        zneg_flag_ |= DRAW_AXIS_ZNEG;
      }
      else {
        zpos_flag_ |= DRAW_AXIS_ZNEG;
        zneg_flag_ |= DRAW_AXIS_Z;
      }

      zplane_axes_.x = float((zpos_flag_ & (PLANE_XZ | PLANE_XY)) != 0);
      zplane_axes_.y = float((zpos_flag_ & (PLANE_YZ | PLANE_XY)) != 0);
      zplane_axes_.z = float((zpos_flag_ & (PLANE_YZ | PLANE_XZ)) != 0);
    }

    data_.size = float4(v3d_clip_end_);
    if (!view.is_persp()) {
      data_.size /= min_ff(fabsf(view.winmat()[0][0]), fabsf(view.winmat()[1][1]));
    }

    data_.distance = v3d_clip_end_ / 2.0f;

    if (is_xr_) {
      /* The calculations for the grid parameters assume that the view matrix has no scale
       * component, which may not be correct if the user is "shrunk" or "enlarged" by zooming in or
       * out. Therefore, we need to compensate the values here. */
      /* Assumption is uniform scaling (all column vectors are of same length). */
      float viewinvscale = len_v3(view.viewinv()[0]);
      data_.distance *= viewinvscale;
    }
  }
};

}  // namespace blender::draw::overlay
