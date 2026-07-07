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
 * Text objects related overlays.
 * Currently only display cursor and selection of text edit mode.
 */
class Text : Overlay {

 private:
  PassSimple ps_ = {"TextEdit"};
  PassSimple::Sub *selection_ps_ = nullptr;
  PassSimple::Sub *selection_highlight_ps_ = nullptr;
  PassSimple::Sub *cursor_ps_ = nullptr;

  View view_edit_text = {"view_edit_text"};

  LinePrimitiveBuf box_line_buf_;

  /** A solid quad. */
  gpu::Batch *quad = nullptr;
  /** A wire quad. */
  gpu::Batch *quad_wire = nullptr;

 public:
  Text(SelectionType selection_type) : box_line_buf_(selection_type, "box_line_buf_") {}

  void begin_sync(Resources &res, const State &state) final;

  void edit_object_sync(Manager &manager,
                        const ObjectRef &ob_ref,
                        Resources &res,
                        const State & /*state*/) final;

  void end_sync(Resources &res, const State &state) final;

  void draw(Framebuffer &framebuffer, Manager &manager, View &view) final;

 private:
  void add_select(Manager &manager, const Curve &cu, const float4x4 &ob_to_world);

  void add_cursor(Manager &manager, const Curve &cu, const float4x4 &ob_to_world);

  void add_boxes(const Resources &res, const Curve &cu, const float4x4 &ob_to_world);
};

}  // namespace blender::draw::overlay
