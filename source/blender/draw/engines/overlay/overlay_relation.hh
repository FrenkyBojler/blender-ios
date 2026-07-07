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
 * Display object relations as dashed lines.
 * Covers parenting relationships and constraints.
 */
class Relations : Overlay {

 private:
  PassSimple ps_ = {"Relations"};

  LinePrimitiveBuf relations_buf_;
  PointPrimitiveBuf points_buf_;

 public:
  Relations(SelectionType selection_type)
      : relations_buf_(selection_type, "relations_buf_"),
        points_buf_(selection_type, "points_buf_")
  {
  }

  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void end_sync(Resources &res, const State &state) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
