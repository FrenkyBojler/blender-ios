/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "draw_cache_impl.hh"

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Curve object display (including legacy curves) for both object and edit modes.
 */
class Curves : Overlay {
 private:
  PassSimple edit_curves_ps_ = {"Curve Edit"};
  PassSimple::Sub *edit_curves_lines_ = nullptr;

  PassSimple edit_curves_handles_ps_ = {"Curve Edit Handles"};
  PassSimple::Sub *edit_curves_points_ = nullptr;
  PassSimple::Sub *edit_curves_handles_ = nullptr;

  PassSimple edit_legacy_curve_ps_ = {"Legacy Curve Edit"};
  PassSimple::Sub *edit_legacy_curve_wires_ = nullptr;
  PassSimple::Sub *edit_legacy_curve_normals_ = nullptr;

  PassSimple edit_legacy_curve_handles_ps_ = {"Legacy Curve Edit Handles"};
  PassSimple::Sub *edit_legacy_curve_points_ = nullptr;
  PassSimple::Sub *edit_legacy_curve_handles_ = nullptr;

  PassSimple edit_legacy_surface_handles_ps = {"Surface Edit"};
  PassSimple::Sub *edit_legacy_surface_handles_ = nullptr;
  /* Handles that are below the geometry and are rendered with lower alpha. */
  PassSimple::Sub *edit_legacy_surface_xray_handles_ = nullptr;

  /* TODO(fclem): This is quite wasteful and expensive, prefer in shader Z modification like the
   * retopology offset. */
  View view_edit_cage = {"view_edit_cage"};
  View::OffsetData offset_data_;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void edit_object_sync(Manager &manager,
                        const ObjectRef &ob_ref,
                        Resources & /*res*/,
                        const State & /*state*/) final;

  void edit_object_sync_legacy(Manager &manager, const ObjectRef &ob_ref, Resources & /*res*/);

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;

  void draw_color_only(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
