/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#pragma once

#include "BLI_map.hh"

#include "ufbx.h"

struct Object;
struct Key;
struct Material;

namespace blender::io::fbx {

const char *get_fbx_name(const ufbx_string &name, const char *def = "Untitled");

struct FbxElementMapping {
  Map<const ufbx_element *, Object *> el_to_object;
  Map<const ufbx_element *, Key *> el_to_shape_key;
  Map<const ufbx_material *, Material *> mat_to_material;
};

}  // namespace blender::io::fbx
