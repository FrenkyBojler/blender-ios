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
 * Displays geometry node viewer output.
 * Values are displayed as vertex or face colors on top of the active object.
 */
class AttributeViewer : Overlay {
 private:
  PassMain ps_ = {"attribute_viewer_ps_"};

  PassMain::Sub *mesh_sub_ = nullptr;
  PassMain::Sub *pointcloud_sub_ = nullptr;
  PassMain::Sub *curve_sub_ = nullptr;
  PassMain::Sub *curves_sub_ = nullptr;
  PassMain::Sub *instance_sub_ = nullptr;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final;

  void pre_draw(Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    manager.generate_commands(ps_, view);
  }

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    if (!enabled_) {
      return;
    }

    GPU_framebuffer_bind(framebuffer);
    manager.submit_only(ps_, view);
  }

 private:
  void populate_for_instance(const ObjectRef &ob_ref, const State &state, Manager &manager);

  void populate_for_geometry(const ObjectRef &ob_ref, const State &state, Manager &manager);
};
}  // namespace blender::draw::overlay
