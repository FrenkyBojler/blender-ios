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
 * Display object origins as dots.
 * The option can be found under (Viewport Overlays > Objects > Origins).
 */
class Origins : Overlay {
 private:
  StorageVectorBuffer<VertexData> point_buf_;
  select::SelectBuf select_buf_;

  PassSimple ps_ = {"Origins"};

 public:
  Origins(SelectionType selection_type) : select_buf_(selection_type) {}

  void begin_sync(Resources & /*res*/, const State &state) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void end_sync(Resources &res, const State &state) final;

  void draw_color_only(Framebuffer &framebuffer, Manager &manager, View &view) final;
};
}  // namespace blender::draw::overlay
