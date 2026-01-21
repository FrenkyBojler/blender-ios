/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

#include "AS_asset_library.hh"

namespace blender {
struct bUserAssetLibrary;
}

namespace blender::asset_system {

class RemoteAssetLibrary : public AssetLibrary {
  std::string remote_url_;

 public:
  static void refresh_cache_directory_name(const bUserAssetLibrary &library_definition);
  /**
   * Looks up the #bUserAssetLibrary from the URL and name of the library and calls the other
   * override.
   *
   * Called before requesting any resource of the library to download, to ensure it will download
   * to the right location, even if something (like another B.ender) messed with the name.
   */
  void refresh_cache_directory_name() const;

  RemoteAssetLibrary(StringRef remote_url, StringRef name, StringRef cache_rootpath);
  std::optional<AssetLibraryReference> library_reference() const override;
  std::optional<StringRefNull> remote_url() const override;
  void refresh_catalogs() override;
  void load_or_reload_catalogs();
};

}  // namespace blender::asset_system
