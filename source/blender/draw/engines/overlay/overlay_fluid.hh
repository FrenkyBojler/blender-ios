/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "overlay_base.hh"

namespace blender {
struct FluidDomainSettings;
}

namespace blender::draw::overlay {

/**
 * Draw fluid simulation overlays (water, smoke).
 */
class Fluids : Overlay {
 private:
  const SelectionType selection_type_;

  PassSimple fluid_ps_ = {"fluid_ps_"};
  PassSimple::Sub *velocity_needle_ps_ = nullptr;
  PassSimple::Sub *velocity_mac_ps_ = nullptr;
  PassSimple::Sub *velocity_streamline_ps_ = nullptr;
  PassSimple::Sub *grid_lines_flags_ps_ = nullptr;
  PassSimple::Sub *grid_lines_flat_ps_ = nullptr;
  PassSimple::Sub *grid_lines_range_ps_ = nullptr;

  ShapeInstanceBuf<ExtraInstanceData> cube_buf_ = {selection_type_, "cube_buf_"};

  int dominant_axis = -1;

 public:
  Fluids(const SelectionType selection_type) : selection_type_(selection_type) {};

  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void end_sync(Resources &res, const State & /*state*/) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;

 private:
  /* Return axis index or -1 if no slice. */
  int slide_axis_get(const FluidDomainSettings &fluid_domain_settings) const;
};

}  // namespace blender::draw::overlay
