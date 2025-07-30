/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

#include "BLI_string_ref.hh"

namespace blender::asset_system {

StringRefNull essentials_directory_path();

/**
 * This updates the default import method for essentials based on whether packed data-blocks are
 * supported. This can be removed once packed data-blocks are always supported.
 */
void essentials_update_import_method();

}  // namespace blender::asset_system
