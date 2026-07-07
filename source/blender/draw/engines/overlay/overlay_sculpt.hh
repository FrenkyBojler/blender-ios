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
 * Display sculpt modes overlays.
 * Covers face sets and mask for meshes.
 * Draw curve cages (curve guides) for curve sculpting.
 */
class Sculpts : Overlay {

 private:
  PassSimple sculpt_mask_ = {"SculptMaskAndFaceSet"};
  PassSimple::Sub *mesh_ps_ = nullptr;
  PassSimple::Sub *curves_ps_ = nullptr;

  PassSimple sculpt_curve_cage_ = {"SculptCage"};

  bool show_curves_cage_ = false;
  bool show_face_set_ = false;
  bool show_mask_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final;

  void curves_sync(Manager &manager, const ObjectRef &ob_ref, const State &state);

  void mesh_sync(Manager &manager, const ObjectRef &ob_ref, const State &state);

  void draw_line(Framebuffer &framebuffer, Manager &manager, View &view) final;

  void draw_on_render(gpu::FrameBuffer *framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
