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
 * Draw light probe objects.
 */
class LightProbes : Overlay {
  using LightProbeInstanceBuf = ShapeInstanceBuf<ExtraInstanceData>;
  using GroundLineInstanceBuf = ShapeInstanceBuf<float4>;
  using DotsInstanceBuf = ShapeInstanceBuf<float4x4>;

 private:
  const SelectionType selection_type_;

  PassSimple ps_ = {"LightProbes"};
  PassMain ps_dots_ = {"LightProbesDots"};

  struct CallBuffers {
    const SelectionType selection_type_;

    GroundLineInstanceBuf ground_line_buf = {selection_type_, "ground_line_buf"};
    LightProbeInstanceBuf probe_cube_buf = {selection_type_, "probe_cube_buf"};
    LightProbeInstanceBuf probe_planar_buf = {selection_type_, "probe_planar_buf"};
    LightProbeInstanceBuf probe_grid_buf = {selection_type_, "probe_grid_buf"};
    LightProbeInstanceBuf quad_solid_buf = {selection_type_, "quad_solid_buf"};
    LightProbeInstanceBuf cube_buf = {selection_type_, "cube_buf"};
    LightProbeInstanceBuf sphere_buf = {selection_type_, "sphere_buf"};
    LightProbeInstanceBuf single_arrow_buf = {selection_type_, "single_arrow_buf"};

  } call_buffers_{selection_type_};

 public:
  LightProbes(const SelectionType selection_type) : selection_type_(selection_type) {};

  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void end_sync(Resources &res, const State &state) final;

  void pre_draw(Manager &manager, View &view) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;

  void draw_color_only(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
