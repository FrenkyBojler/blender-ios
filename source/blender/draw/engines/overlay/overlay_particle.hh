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
 * Display particle system overlays.
 * Covers particle edit and the legacy hair system.
 */
class Particles : Overlay {
 private:
  PassMain particle_ps_ = {"particle_ps_"};
  PassMain::Sub *dot_ps_ = nullptr;
  PassMain::Sub *shape_ps_ = nullptr;
  PassMain::Sub *hair_ps_ = nullptr;

  PassSimple edit_particle_ps_ = {"edit_particle_ps_"};
  PassSimple::Sub *edit_vert_ps_ = nullptr;
  PassSimple::Sub *edit_edge_ps_ = nullptr;

  bool show_weight_ = false;
  bool show_point_inner_ = false;
  bool show_point_tip_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void edit_object_sync(Manager &manager,
                        const ObjectRef &ob_ref,
                        Resources & /*res*/,
                        const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void pre_draw(Manager &manager, View &view) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;

  void draw(Framebuffer &framebuffer, Manager &manager, View &view) final;
};
}  // namespace blender::draw::overlay
