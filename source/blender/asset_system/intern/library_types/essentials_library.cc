/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup asset_system
 */

#include "AS_remote_library.hh"

#include "BKE_appdir.hh"

#include "BLI_string_ref.hh"

#include "DNA_asset_types.h"
#include "DNA_userdef_types.h"

#include "on_disk_library.hh"
#include "remote_library.hh"
#include "utils.hh"

#include "AS_essentials_library.hh"
#include "essentials_library.hh"

namespace blender::asset_system {

EssentialsAssetLibrary::EssentialsAssetLibrary()
    : OnDiskAssetLibrary(ASSET_LIBRARY_ESSENTIALS,
                         {},
                         utils::normalize_directory_path(essentials_directory_path()),
                         AssetCatalogService::read_only_tag{})
{
  import_method_ = ASSET_IMPORT_PACK;
  if (U.experimental.no_data_block_packing) {
    import_method_ = ASSET_IMPORT_APPEND_REUSE;
  }
}

std::optional<AssetLibraryReference> EssentialsAssetLibrary::library_reference() const
{
  AssetLibraryReference library_ref{};
  library_ref.custom_library_index = -1;
  library_ref.type = ASSET_LIBRARY_ESSENTIALS;
  return library_ref;
}

void EssentialsAssetLibrary::update_default_import_method()
{
  import_method_ = ASSET_IMPORT_PACK;
  if (U.experimental.no_data_block_packing) {
    import_method_ = ASSET_IMPORT_APPEND_REUSE;
  }
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

/* -------------------------------------------------------------------- */
/** \name Online Essentials Library
 *
 * Internally this is a separate library. To the user, it's part of the normal Essentials library.
 * \{ */

StringRefNull online_essentials_cache_directory_path()
{
  static std::string path = []() {
    return remote_library_cache_directory_path("online-essentials");
  }();
  return path;
}

StringRefNull online_essentials_url()
{
  return OnlineEssentialsLibrary::URL;
}

bool is_online_essentials_url(const StringRef url)
{
  if (url.is_empty()) {
    return false;
  }

  if (remote_library_url_ends_with_top_meta_file_name(url)) {
    BLI_assert(url.drop_suffix(REMOTE_LIBRARY_TOP_META_FILE_NAME.size()).back() == '/');
    return url.drop_suffix(REMOTE_LIBRARY_TOP_META_FILE_NAME.size()) ==
           OnlineEssentialsLibrary::URL;
  }

  return url == OnlineEssentialsLibrary::URL;
}

OnlineEssentialsLibrary::OnlineEssentialsLibrary()
    : RemoteAssetLibrary(URL, "Online Essentials", online_essentials_cache_directory_path())
{
}

std::optional<AssetLibraryReference> OnlineEssentialsLibrary::library_reference() const
{
  AssetLibraryReference library_ref{};
  library_ref.type = ASSET_LIBRARY_ONLINE_ESSENTIALS;
  library_ref.custom_library_index = -1;
  return library_ref;
}

void OnlineEssentialsLibrary::update_default_import_method()
{
  import_method_ = ASSET_IMPORT_PACK;
  if (U.experimental.no_data_block_packing) {
    import_method_ = ASSET_IMPORT_APPEND_REUSE;
  }
}

/** \} */

}  // namespace blender::asset_system
