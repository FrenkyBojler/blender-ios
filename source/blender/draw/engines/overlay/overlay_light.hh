/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "overlay_base.hh"

namespace blender::draw::overlay {

class Lights : Overlay {
  using LightInstanceBuf = ShapeInstanceBuf<ExtraInstanceData>;
  using GroundLineInstanceBuf = ShapeInstanceBuf<float4>;

 private:
  const SelectionType selection_type_;

  PassSimple ps_ = {"Lights"};

  struct CallBuffers {
    const SelectionType selection_type_;
    GroundLineInstanceBuf ground_line_buf = {selection_type_, "ground_line_buf"};
    LightInstanceBuf icon_inner_buf = {selection_type_, "icon_inner_buf"};
    LightInstanceBuf icon_outer_buf = {selection_type_, "icon_outer_buf"};
    LightInstanceBuf icon_sun_rays_buf = {selection_type_, "icon_sun_rays_buf"};
    LightInstanceBuf point_buf = {selection_type_, "point_buf"};
    LightInstanceBuf sun_buf = {selection_type_, "sun_buf"};
    LightInstanceBuf spot_buf = {selection_type_, "spot_buf"};
    LightInstanceBuf spot_cone_back_buf = {selection_type_, "spot_cone_back_buf"};
    LightInstanceBuf spot_cone_front_buf = {selection_type_, "spot_cone_front_buf"};
    LightInstanceBuf area_disk_buf = {selection_type_, "area_disk_buf"};
    LightInstanceBuf area_square_buf = {selection_type_, "area_square_buf"};
  } call_buffers_{selection_type_};

 public:
  Lights(const SelectionType selection_type) : selection_type_(selection_type) {};

  void begin_sync(Resources & /*res*/, const State &state) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void end_sync(Resources &res, const State &state) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
