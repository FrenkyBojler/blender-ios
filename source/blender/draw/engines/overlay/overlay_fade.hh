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
 * Fades surfaces not currently in the active edit mode.
 * Can be toggle in (Viewport Overlays > Geometry > Fade Inactive Geometry)
 */
class Fade : Overlay {
 private:
  PassMain ps_ = {"FadeGeometry"};

  PassMain::Sub *mesh_fade_geometry_ps_;
  /* Passes for Pose Fade Geometry. */
  PassMain::Sub *armature_fade_geometry_active_ps_;
  PassMain::Sub *armature_fade_geometry_other_ps_;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final;

  void pre_draw(Manager &manager, View &view) final;

  void draw(Framebuffer &framebuffer, Manager &manager, View &view) final;

 private:
};
}  // namespace blender::draw::overlay
