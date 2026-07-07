/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "overlay_base.hh"

namespace blender::draw::overlay {

/* Add prepass which will write to the depth buffer so that the
 * alpha-under overlays (alpha checker) will draw correctly for external engines.
 * NOTE: Use the same Z-depth value as in the regular image drawing engine. */
class ImagePrepass : Overlay {
 private:
  PassSimple ps_ = {"ImagePrepass"};

 public:
  void begin_sync(Resources &res, const State &state) final;

  void draw_on_render(gpu::FrameBuffer *framebuffer, Manager &manager, View &view) final;
};

/**
 * A depth pass that write surface depth when it is needed.
 * It is also used for selecting non overlay-only objects.
 */
class Prepass : Overlay {
 private:
  PassMain ps_ = {"prepass"};
  PassMain::Sub *mesh_ps_ = nullptr;
  PassMain::Sub *mesh_flat_ps_ = nullptr;
  PassMain::Sub *hair_ps_ = nullptr;
  PassMain::Sub *curves_ps_ = nullptr;
  PassMain::Sub *pointcloud_ps_ = nullptr;
  PassMain::Sub *grease_pencil_ps_ = nullptr;

  bool use_material_slot_selection_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void particle_sync(Manager &manager,
                     const ObjectRef &ob_ref,
                     Resources &res,
                     const State &state);

  void sculpt_sync(Manager &manager, const ObjectRef &ob_ref, Resources &res);

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void pre_draw(Manager &manager, View &view) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
