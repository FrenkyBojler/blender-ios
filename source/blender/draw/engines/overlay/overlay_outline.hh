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
 * Display selected object outline.
 * The option can be found under (Viewport Overlays > Objects > Outline Selected).
 */
class Outline : Overlay {
 private:
  /* Simple render pass that renders an object ID pass. */
  PassMain outline_prepass_ps_ = {"Prepass"};
  PassMain::Sub *prepass_curves_ps_ = nullptr;
  PassMain::Sub *prepass_pointcloud_ps_ = nullptr;
  PassMain::Sub *prepass_gpencil_ps_ = nullptr;
  PassMain::Sub *prepass_mesh_ps_ = nullptr;
  PassMain::Sub *prepass_volume_ps_ = nullptr;
  PassMain::Sub *prepass_wire_ps_ = nullptr;
  /* Detect edges inside the ID pass and output color for each of them. */
  PassSimple outline_resolve_ps_ = {"Resolve"};

  TextureFromPool object_id_tx_ = {"outline_ob_id_tx"};
  TextureFromPool tmp_depth_tx_ = {"outline_depth_tx"};

  Framebuffer prepass_fb_ = {"outline.prepass_fb"};

  Vector<FlatObjectRef> flat_objects_;

  PassMain outline_prepass_flat_ps_ = {"PrepassFlat"};

 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  /* Flat objects outline workaround need to generate passes for each redraw. */
  void flat_objects_pass_sync(Manager &manager, View &view, Resources &res, const State &state);

  void pre_draw(Manager &manager, View &view) final;

  /* TODO(fclem): Remove dependency on Resources. */
  void draw_line_only_ex(Framebuffer &framebuffer, Resources &res, Manager &manager, View &view);
};

}  // namespace blender::draw::overlay
