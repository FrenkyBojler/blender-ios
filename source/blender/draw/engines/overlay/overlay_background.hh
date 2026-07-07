/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Draw background color .
 */
class Background : Overlay {
 private:
  PassSimple bg_ps_ = {"Background"};
  PassSimple bg_vignette_ps_ = {"Background Vignette"};

  gpu::FrameBuffer *framebuffer_ref_ = nullptr;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void draw_output(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    framebuffer_ref_ = framebuffer;
    manager.submit(bg_ps_, view);
  }

  void draw_vignette(Framebuffer &framebuffer, Manager &manager, View &view)
  {
    framebuffer_ref_ = framebuffer;
    manager.submit(bg_vignette_ps_, view);
  }
};

}  // namespace blender::draw::overlay
