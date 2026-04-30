/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

#include "BLI_string_ref.hh"

namespace blender {
class UUID;
}

namespace blender::asset_system {

StringRefNull essentials_directory_path();

bool skip_experimental_asset_catalog(const UUID &catalog_id);

}  // namespace blender::asset_system
