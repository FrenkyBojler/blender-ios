/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#include "BKE_appdir.hh"

#include "utils.hh"

#include "DNA_userdef_types.h"

#include "AS_essentials_library.hh"
#include "essentials_library.hh"

namespace blender::asset_system {

EssentialsAssetLibrary::EssentialsAssetLibrary()
    : OnDiskAssetLibrary(ASSET_LIBRARY_ESSENTIALS,
                         {},
                         utils::normalize_directory_path(essentials_directory_path()),
                         /*is_read_only=*/true)
{
}

std::optional<AssetLibraryReference> EssentialsAssetLibrary::library_reference() const
{
  AssetLibraryReference library_ref{};
  library_ref.custom_library_index = -1;
  library_ref.type = ASSET_LIBRARY_ESSENTIALS;
  return library_ref;
}

std::optional<eAssetImportMethod> EssentialsAssetLibrary::import_method() const
{
  if (U.experimental.no_data_block_packing) {
    return ASSET_IMPORT_APPEND_REUSE;
  }
  return ASSET_IMPORT_PACK;
}

StringRefNull essentials_directory_path()
{
  static std::string path = []() {
    const std::optional<std::string> datafiles_path = BKE_appdir_folder_id(
        BLENDER_SYSTEM_DATAFILES, "assets");
    return datafiles_path.value_or("");
  }();
  return path;
}

bool skip_experimental_asset_catalog(const UUID &catalog_id)
{
  const UUID UUID_hair_dynamics("1cc346fb-c338-d030-adf0-5c8e43bd5e4a");

  /* Enable catalog for hair dynamics only if the feature is enabled. */
  if (!U.experimental.use_geometry_nodes_hair_dynamics && catalog_id == UUID_hair_dynamics) {
    return true;
  }
  return false;
}

}  // namespace blender::asset_system
