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
 * Draw force fields.
 * Controlled by (Physics > Force Field)
 */
class ForceFields : Overlay {
  using ForceFieldsInstanceBuf = ShapeInstanceBuf<ExtraInstanceData>;

 private:
  PassSimple ps_ = {"ForceFields"};

  struct CallBuffers {
    const SelectionType selection_type_;

    ForceFieldsInstanceBuf field_force_buf = {selection_type_, "field_force_buf"};
    ForceFieldsInstanceBuf field_wind_buf = {selection_type_, "field_wind_buf"};
    ForceFieldsInstanceBuf field_vortex_buf = {selection_type_, "field_vortex_buf"};
    ForceFieldsInstanceBuf field_curve_buf = {selection_type_, "field_curve_buf"};
    ForceFieldsInstanceBuf field_sphere_limit_buf = {selection_type_, "field_sphere_limit_buf"};
    ForceFieldsInstanceBuf field_tube_limit_buf = {selection_type_, "field_tube_limit_buf"};
    ForceFieldsInstanceBuf field_cone_limit_buf = {selection_type_, "field_cone_limit_buf"};
  } call_buffers_;

 public:
  ForceFields(const SelectionType selection_type) : call_buffers_{selection_type} {}

  void begin_sync(Resources & /*res*/, const State & /*state*/) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void end_sync(Resources &res, const State &state) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
