/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

#include "BLI_string_ref.hh"

#include "on_disk_library.hh"
#include "remote_library.hh"

namespace blender::asset_system {

class EssentialsAssetLibrary : public OnDiskAssetLibrary {
 public:
  EssentialsAssetLibrary();

  std::optional<AssetLibraryReference> library_reference() const override;

  /** Update the default import method based on whether packed data-blocks are supported. */
  void update_default_import_method();
};

class OnlineEssentialsLibrary : public RemoteAssetLibrary {
 public:
  OnlineEssentialsLibrary();

  /* Trailing slash matters! */
  static constexpr StringRefNull URL =
      "https://cdn.extensions.blender.org/asset-libraries/essentials/";

  std::optional<AssetLibraryReference> library_reference() const override;

  /** Update the default import method based on whether packed data-blocks are supported. */
  void update_default_import_method();
};

}  // namespace blender::asset_system
