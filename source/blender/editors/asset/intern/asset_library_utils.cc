/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include "BKE_context.hh"
#include "BKE_preferences.h"

#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_shelf.hh"

#include "BLI_listbase.h"
#include "BLI_string_ref.hh"

#include "DNA_userdef_types.h"
#include "RNA_access.hh"
#include "UI_resources.hh"
#include "WM_api.hh"

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"

namespace blender::ed::asset {

static asset_system::AssetCatalog &library_ensure_catalog(
    asset_system::AssetLibrary &library, const asset_system::AssetCatalogPath &path)
{
  if (asset_system::AssetCatalog *catalog = library.catalog_service().find_catalog_by_path(path)) {
    return *catalog;
  }
  return *library.catalog_service().create_catalog(path);
}

/* Suppress warning for GCC-14.2. This isn't a dangling reference
 * because the #asset_system::AssetLibrary owns the returned value. */
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdangling-reference"
#endif

blender::asset_system::AssetCatalog &library_ensure_catalogs_in_path(
    asset_system::AssetLibrary &library, const blender::asset_system::AssetCatalogPath &path)
{
  /* Adding multiple catalogs in a path at a time with #AssetCatalogService::create_catalog()
   * doesn't work; add each potentially new catalog in the hierarchy manually here. */
  asset_system::AssetCatalogPath parent = "";
  path.iterate_components([&](StringRef component_name, bool /*is_last_component*/) {
    library_ensure_catalog(library, parent / component_name);
    parent = parent / component_name;
  });
  return *library.catalog_service().find_catalog_by_path(path);
}

#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif

AssetLibraryReference user_library_to_library_ref(const bUserAssetLibrary &user_library)
{
  AssetLibraryReference library_ref{};
  library_ref.custom_library_index = BLI_findindex(&U.asset_libraries, &user_library);
  library_ref.type = ASSET_LIBRARY_CUSTOM;
  return library_ref;
}

const bUserAssetLibrary *library_ref_to_user_library(const AssetLibraryReference &library_ref)
{
  if (library_ref.type != ASSET_LIBRARY_CUSTOM) {
    return nullptr;
  }
  return static_cast<const bUserAssetLibrary *>(
      BLI_findlink(&U.asset_libraries, library_ref.custom_library_index));
}

const bUserAssetLibrary *get_asset_library_from_prop(PointerRNA &ptr)
{
  const int enum_value = RNA_enum_get(&ptr, "asset_library_reference");
  const AssetLibraryReference lib_ref = asset::library_reference_from_enum_value(enum_value);
  return BKE_preferences_asset_library_find_index(&U, lib_ref.custom_library_index);
}

void visit_library_catalogs_catalog_for_search(
    const Main &bmain,
    const AssetLibraryReference lib,
    const StringRef edit_text,
    const FunctionRef<void(StringPropertySearchVisitParams)> visit_fn)
{
  const asset_system::AssetLibrary *library = AS_asset_library_load(&bmain, lib);
  if (!library) {
    return;
  }

  if (!edit_text.is_empty()) {
    const asset_system::AssetCatalogPath edit_path = edit_text;
    if (!library->catalog_service().find_catalog_by_path(edit_path)) {
      visit_fn(StringPropertySearchVisitParams{edit_path.str(), std::nullopt, ICON_ADD});
    }
  }

  const asset_system::AssetCatalogTree &full_tree = library->catalog_service().catalog_tree();
  full_tree.foreach_item([&](const asset_system::AssetCatalogTreeItem &item) {
    visit_fn(StringPropertySearchVisitParams{item.catalog_path().str(), std::nullopt});
  });
}

void refresh_asset_library(const bContext *C, const AssetLibraryReference &library_ref)
{
  asset::list::clear(&library_ref, C);
  /* TODO: Should the all library reference be automatically cleared? */
  AssetLibraryReference all_lib_ref = asset_system::all_library_reference();
  asset::list::clear(&all_lib_ref, C);
}

void refresh_asset_library(const bContext *C, const bUserAssetLibrary &user_library)
{
  refresh_asset_library(C, user_library_to_library_ref(user_library));
}

void show_catalog_in_asset_shelf(const bContext &C, const StringRefNull catalog_path)
{
  wmWindowManager *wm = CTX_wm_manager(&C);
  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
    const bScreen *screen = WM_window_get_active_screen(win);
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      const AssetShelf *shelf = asset::shelf::active_shelf_from_area(area);
      if (shelf && BKE_preferences_asset_shelf_settings_ensure_catalog_path_enabled(
                       &U, shelf->idname, catalog_path.c_str()))
      {
        U.runtime.is_dirty = true;
      }
    }
  }
}

}  // namespace blender::ed::asset
