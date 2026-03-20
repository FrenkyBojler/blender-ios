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
StringRefNull online_essentials_cache_directory_path();
StringRefNull online_essentials_url();

/**
 * Check if the given URL matches the online essentials URL, with or without the optional
 * "_asset-library-meta.json" ending. If the .json file name ending isn't present, the trailing
 * slash is necessary for the URLs to match.
 */
bool is_online_essentials_url(StringRef url);

}  // namespace blender::asset_system
