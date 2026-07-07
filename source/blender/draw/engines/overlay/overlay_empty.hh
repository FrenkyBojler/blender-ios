/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "DRW_render.hh"
#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Empty object type drawing, including image empties.
 */
class Empties : Overlay {
  friend class Cameras;
  using EmptyInstanceBuf = ShapeInstanceBuf<ExtraInstanceData>;

 private:
  /* Images added by Image > Background. Both added in preset view (like Top, Front, ..) and in
   * custom view. Object property "In Front" unchecked. */
  PassSortable images_back_ps_ = {"images_back_ps_"};
  /* All Empty images from cases of `images_ps_`, `images_blend_ps_`, `images_back_ps_`
   * with object property "In Front" checked. */
  PassSortable images_front_ps_ = {"images_front_ps_"};

  /* Images added by Empty > Image and Image > Reference with unchecked image "Opacity".
   * Object property "In Front" unchecked. */
  PassMain images_ps_ = {"images_ps_"};
  /* Images added by Empty > Image and Image > Reference with image "Opacity" checked.
   * Object property "In Front" unchecked. */
  PassSortable images_blend_ps_ = {"images_blend_ps_"};

  PassSimple ps_ = {"Empties"};

  struct CallBuffers {
    const SelectionType selection_type_;
    EmptyInstanceBuf plain_axes_buf = {selection_type_, "plain_axes_buf"};
    EmptyInstanceBuf single_arrow_buf = {selection_type_, "single_arrow_buf"};
    EmptyInstanceBuf cube_buf = {selection_type_, "cube_buf"};
    EmptyInstanceBuf circle_buf = {selection_type_, "circle_buf"};
    EmptyInstanceBuf sphere_buf = {selection_type_, "sphere_buf"};
    EmptyInstanceBuf cone_buf = {selection_type_, "cone_buf"};
    EmptyInstanceBuf arrows_buf = {selection_type_, "arrows_buf"};
    EmptyInstanceBuf image_buf = {selection_type_, "image_buf"};
  } call_buffers_;

  View::OffsetData offset_data_;
  float4x4 depth_bias_winmat_;

 public:
  Empties(const SelectionType selection_type) : call_buffers_{selection_type} {};

  void begin_sync(Resources &res, const State &state) final;

  static void begin_sync(CallBuffers &call_buffers);

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  static void object_sync(const select::ID select_id,
                          const float4x4 &matrix,
                          const float draw_size,
                          const char empty_drawtype,
                          const float4 &color,
                          CallBuffers &call_buffers);

  void end_sync(Resources &res, const State &state) final;

  static void end_sync(Resources &res,
                       const State &state,
                       PassSimple::Sub &ps,
                       CallBuffers &call_buffers);

  void pre_draw(Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    manager.generate_commands(images_back_ps_, view);
    manager.generate_commands(images_ps_, view);
    manager.generate_commands(images_blend_ps_, view);
    manager.generate_commands(images_front_ps_, view);

    depth_bias_winmat_ = offset_data_.winmat_polygon_offset(view.winmat(), -1.0f);
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    GPU_framebuffer_bind(framebuffer);
    manager.submit(ps_, view);
  }

  void draw_background_images(Framebuffer &framebuffer, Manager &manager, View &view)
  {
    if (!enabled_) {
      return;
    }

    GPU_framebuffer_bind(framebuffer);
    manager.submit_only(images_back_ps_, view);
  }

  void draw_images(Framebuffer &framebuffer, Manager &manager, View &view)
  {
    if (!enabled_) {
      return;
    }

    GPU_framebuffer_bind(framebuffer);

    manager.submit_only(images_ps_, view);
    manager.submit_only(images_blend_ps_, view);
  }

  void draw_in_front_images(Framebuffer &framebuffer, Manager &manager, View &view)
  {
    if (!enabled_) {
      return;
    }

    GPU_framebuffer_bind(framebuffer);

    manager.submit_only(images_front_ps_, view);
  }

 private:
  void image_sync(const ObjectRef &ob_ref,
                  select::ID select_id,
                  Manager &manager,
                  Resources &res,
                  const State &state,
                  EmptyInstanceBuf &empty_image_buf);

  PassMain::Sub &create_subpass(const State &state,
                                const Object &ob,
                                const bool use_alpha_blend,
                                const float4x4 &mat,
                                Resources &res);

  PassMain::Sub &create_subpass(const State &state,
                                const float4x4 &mat,
                                Resources &res,
                                PassSortable &parent,
                                bool depth_bias);
};

}  // namespace blender::draw::overlay
