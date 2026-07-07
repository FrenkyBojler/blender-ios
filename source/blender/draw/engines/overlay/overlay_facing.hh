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
 * Draw a specific color for front and back-faces on surfaces.
 * Can be toggle in (Viewport Overlays > Geometry > Face Orientation)
 */
class Facing : Overlay {

 private:
  PassMain ps_ = {"Facing"};

 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final;

  void pre_draw(Manager &manager, View &view) final;

  void draw(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
