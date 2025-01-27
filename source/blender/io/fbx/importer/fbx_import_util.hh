/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup fbx
 */

#pragma once

struct ufbx_string;

namespace blender::io::fbx {

const char *get_fbx_name(const ufbx_string &name, const char *def = "Untitled");

}  // namespace blender::io::fbx
