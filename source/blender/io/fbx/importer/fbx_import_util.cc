/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#include "ufbx.h"

namespace blender::io::fbx {

const char *get_fbx_name(const ufbx_string &name, const char *def)
{
  return name.length > 0 ? name.data : def;
}

}  // namespace blender::io::fbx
