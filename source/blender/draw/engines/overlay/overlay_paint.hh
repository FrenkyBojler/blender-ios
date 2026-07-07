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
 * Display paint modes overlays.
 * Covers weight paint, vertex paint and texture paint.
 */
class Paints : Overlay {

 private:
  /* Draw selection state on top of the mesh to communicate which areas can be painted on. */
  PassSimple paint_region_ps_ = {"paint_region_ps_"};
  PassSimple::Sub *paint_region_edge_ps_ = nullptr;
  PassSimple::Sub *paint_region_face_ps_ = nullptr;
  PassSimple::Sub *paint_region_vert_ps_ = nullptr;

  PassSimple weight_ps_ = {"weight_ps_"};
  /* Used when there's not a valid pre-pass (depth <=). */
  PassSimple::Sub *weight_opaque_ps_ = nullptr;
  /* Used when there's a valid pre-pass (depth ==). */
  PassSimple::Sub *weight_masked_transparency_ps_ = nullptr;
  /* Black and white mask overlayed on top of mesh to preview painting influence. */
  PassSimple paint_mask_ps_ = {"paint_mask_ps_"};

  bool show_weight_ = false;
  bool show_wires_ = false;
  bool show_paint_mask_ = false;
  bool masked_transparency_support_ = false;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final;

  void draw(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
