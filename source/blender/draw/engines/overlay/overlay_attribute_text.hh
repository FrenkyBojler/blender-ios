/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 */

#pragma once

#include "overlay_base.hh"

namespace blender::draw::overlay {

/**
 * Displays geometry node viewer output.
 * Values are displayed as text on top of the active object.
 */
class AttributeTexts : Overlay {
 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources & /*res*/,
                   const State &state) final;
};

}  // namespace blender::draw::overlay
