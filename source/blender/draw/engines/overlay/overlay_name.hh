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
 * Display object names next to their origin.
 * The option can be found under (Object > Viewport Display > Show > Name).
 */
class Names : Overlay {
 public:
  void begin_sync(Resources &res, const State &state) final;

  void object_sync(Manager & /*manager*/,
                   const ObjectRef &ob_ref,
                   Resources &res,
                   const State &state) final;
};

}  // namespace blender::draw::overlay
