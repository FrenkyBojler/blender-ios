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
 * Draw 2D or 3D grid as well at global X, Y and Z axes.
 */
class Grid : Overlay {
 private:
  /* Shader data */
  PassSimple grid_ps_ = {"grid_ps_"};
  UniformBuffer<OVERLAY_GridData> grid_ubo_;
  StorageVectorBuffer<float4> tile_pos_buf_;

  /* Config data */
  float2 grid_offs_ = float2(0.0f);
  int grid_flag_ = 0;
  int axis_flag_ = 0;
  uint num_iters_ = 0;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;

 private:
  bool init(const State &state);

  bool init_space_image(const State &state);

  bool init_v3d(const State &state);
};
}  // namespace blender::draw::overlay
