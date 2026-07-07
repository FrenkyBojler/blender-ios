/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Draw wireframe of objects.
 *
 * The object wireframe can be drawn because of:
 * - display option (Object > Viewport Display > Wireframe)
 * - overlay option (Viewport Overlays > Geometry > Wireframe)
 * - display as (Object > Viewport Display > Wire)
 * - wireframe shading mode
 */
class Wireframe : Overlay {
 private:
  PassMain wireframe_ps_ = {"Wireframe"};
  struct ColoringPass {
    PassMain::Sub *curves_ps_ = nullptr;
    PassMain::Sub *mesh_ps_ = nullptr;
    /* Variant for meshes that force drawing all edges. */
    PassMain::Sub *mesh_all_edges_ps_ = nullptr;
    PassMain::Sub *points_ps_ = nullptr;
    PassMain::Sub *pointcloud_ps_ = nullptr;
  } colored, non_colored;

  /* Copy of the depth buffer to be able to read it during wireframe rendering. */
  TextureFromPool tmp_depth_tx_ = {"tmp_depth_tx"};
  bool do_depth_copy_workaround_ = false;

  /* Force display of wireframe on surface objects, regardless of the object display settings. */
  bool show_wire_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync_ex(Manager &manager,
                      const ObjectRef &ob_ref,
                      Resources &res,
                      const State &state,
                      const bool in_edit_paint_mode,
                      const bool in_edit_mode);

  void pre_draw(Manager &manager, View &view) final;

  void copy_depth(TextureRef &depth_tx);

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
