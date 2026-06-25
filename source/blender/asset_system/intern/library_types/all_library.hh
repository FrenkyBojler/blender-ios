/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#pragma once

#include "AS_asset_library.hh"

#include "BLI_cache_mutex.hh"

namespace blender::asset_system {

class AllAssetLibrary : public AssetLibrary {
  /**
   * Guards the lazily (re)built merged catalog service in #catalog_service_. Deduplicates
   * concurrent rebuilds: only one thread merges at a time, others wait for and receive its result.
   */
  CacheMutex catalog_cache_mutex_;

 public:
  AllAssetLibrary();

  void force_remote_listing_download() const override;

  std::optional<AssetLibraryReference> library_reference() const override;
  std::optional<eAssetImportMethod> import_method() const override;
  void refresh_catalogs() override;

  /**
   * Update the available catalog service and catalog tree from the nested asset libraries if
   * #is_catalogs_dirty() is true. Completely recreates the catalog service (invalidating pointers
   * to the previous one).
   *
   * \note This does not (re)load any catalog definition files from disk, it just rebuilds the all
   *     library catalog service to reflect the in-memory state of nested catalog services. To
   *     reload catalog definitions from disk, call #AssetCatalogService::reload_catalogs() for the
   *     corresponding library (won't do anything for the "All" library, since that doesn't have
   *     its own catalog definition file).
   */
  void rebuild_catalogs_from_nested_if_dirty();

  void tag_catalogs_dirty();
  bool is_catalogs_dirty() const;
};

}  // namespace blender::asset_system
