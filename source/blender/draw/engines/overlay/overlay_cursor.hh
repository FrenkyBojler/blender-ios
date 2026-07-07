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
 * Draw the 2D/3D cursor.
 * Controlled by (Overlay > 3D Cursor)
 */
class Cursor : Overlay {
 private:
  PassSimple ps_ = {"Cursor"};

  bool enabled_ = false;

 public:
  Cursor() {}

  void begin_sync(Resources &res, const State &state) final;

  void draw_output(Framebuffer &framebuffer, Manager &manager, View & /*view*/) final;

 private:
  bool is_cursor_visible_3d(const State &state);
};

}  // namespace blender::draw::overlay
