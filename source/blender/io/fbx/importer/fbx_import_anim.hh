/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#pragma once

#include "BLI_map.hh"

struct Object;
struct Main;
struct ufbx_element;
struct ufbx_scene;

namespace blender::io::fbx {

void import_animations(Main &bmain,
                       const ufbx_scene &fbx,
                       const Map<const ufbx_element *, Object *> &element_to_object,
                       const double fps,
                       const float anim_offset);

}  // namespace blender::io::fbx
