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
 * Draw lattice objects in object and edit mode.
 */
class Lattices : Overlay {
 private:
  PassMain ps_ = {"Lattice"};

  PassMain::Sub *lattice_ps_;
  PassMain::Sub *edit_lattice_wire_ps_;
  PassMain::Sub *edit_lattice_point_ps_;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void edit_object_sync(Manager &manager,
                        const ObjectRef &ob_ref,
                        Resources &res,
                        const State & /*state*/) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void pre_draw(Manager &manager, View &view) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
