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
 * Draw object bounds and texture space.
 *
 * The object bound can be drawn because of:
 * - display bounds (Object > Viewport Display > Bounds)
 * - display as (Object > Viewport Display > Display As > Bounds)
 * - rigid body (Physics > Rigid Body > Collision > Shape)
 *
 * Texture space can be modified by (Data > Texture Space)
 * and displayed by (Object > Viewport Display > Texture Space)
 */
class Bounds : Overlay {
  using BoundsInstanceBuf = ShapeInstanceBuf<ExtraInstanceData>;

 private:
  PassSimple ps_ = {"Bounds"};

  struct CallBuffers {
    const SelectionType selection_type_;

    BoundsInstanceBuf box = {selection_type_, "bound_box"};
    BoundsInstanceBuf sphere = {selection_type_, "bound_sphere"};
    BoundsInstanceBuf cylinder = {selection_type_, "bound_cylinder"};
    BoundsInstanceBuf cone = {selection_type_, "bound_cone"};
    BoundsInstanceBuf capsule_body = {selection_type_, "bound_capsule_body"};
    BoundsInstanceBuf capsule_cap = {selection_type_, "bound_capsule_cap"};
  } call_buffers_;

 public:
  Bounds(const SelectionType selection_type) : call_buffers_{selection_type} {}

  void begin_sync(Resources & /*res*/, const State & /*state*/) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void end_sync(Resources &res, const State &state) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;
};
}  // namespace blender::draw::overlay
