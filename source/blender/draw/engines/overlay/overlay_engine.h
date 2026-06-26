/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw_engine
 */

#pragma once

#include "DRW_render.hh"

namespace blender::draw::overlay {

struct Engine : public DrawEngine::Pointer {
  DrawEngine *create_instance() final;

  void draw_in_front_of_passepartout();

  static void free_static();
};

}  // namespace blender::draw::overlay
