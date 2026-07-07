/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "BLI_map.hh"

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Make newly active mesh flash for a brief period of time.
 * This can be triggered using the "Transfer Mode" operator when in any edit mode.
 */
class ModeTransfer : Overlay {
 private:
  PassSimple ps_ = {"ModeTransfer"};

  Map<std::string, float, 1> object_factors_;

  float4 flash_color_;

 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager &manager,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final;

  void draw(Framebuffer &framebuffer, Manager &manager, View &view) final;
};

}  // namespace blender::draw::overlay
