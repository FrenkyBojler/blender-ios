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
 * Draw meta-balls radius overlays.
 */
class Metaballs : Overlay {
  using SphereOutlineInstanceBuf = ShapeInstanceBuf<BoneInstanceData>;

 private:
  const SelectionType selection_type_;

  PassSimple ps_ = {"MetaBalls"};

  SphereOutlineInstanceBuf circle_buf_ = {selection_type_, "metaball_data_buf"};

 public:
  Metaballs(const SelectionType selection_type) : selection_type_(selection_type) {};

  void begin_sync(Resources & /*res*/, const State & /*state*/) final;

  void edit_object_sync(Manager & /*manager*/,
                        const ObjectRef &ob_ref,
                        Resources &res,
                        const State & /*state*/) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;

  void end_sync(Resources &res, const State &state) final;

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
