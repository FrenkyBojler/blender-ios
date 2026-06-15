/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#pragma once

#include "BLI_map.hh"
#include "BLI_string_ref.hh"

#include "attribute_storage_access.hh"

namespace blender::bke::curves {

/* TODO: Comment. Shared between Curves object and Grease Pencil drawings. */
const Map<StringRef, AttrBuiltinInfo> get_builtin_attributes_map();

}  // namespace blender::bke::curves
