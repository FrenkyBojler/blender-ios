/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#pragma once

#include "BLI_string_ref.hh"
#include "DNA_asset_types.h"

struct bUserAssetLibrary;
struct bContext;
struct AssetLibraryReference;
struct EnumPropertyItem;
struct PointerRNA;
struct PropertyRNA;

namespace blender::asset_system {
class AssetCatalog;
class AssetCatalogPath;
}  // namespace blender::asset_system

namespace blender::ed::asset {

/**
 * Return an index that can be used to uniquely identify \a library, assuming
 * that all relevant indices were created with this function.
 */
int library_reference_to_enum_value(const AssetLibraryReference *library);
/**
 * Return an asset library reference matching the index returned by
 * #library_reference_to_enum_value().
 */
AssetLibraryReference library_reference_from_enum_value(int value);
/**
 * Translate all available asset libraries to an RNA enum, whereby the enum values match the result
 * of #library_reference_to_enum_value() for any given library.
 *
 * Since this is meant for UI display, skips non-displayable libraries, that is, libraries with an
 * empty name or path.
 *
 * \param include_generated: Whether to include libraries that are generated and thus cannot be
 *                           written to. Setting this to false means only custom libraries will be
 *                           included, since they are stored on disk with a single root directory,
 *                           thus have a well defined location that can be written to.
 */
const EnumPropertyItem *library_reference_to_rna_enum_itemf(bool include_generated);

/**
 *
 */
blender::asset_system::AssetCatalog &library_ensure_catalogs_in_path(
    blender::asset_system::AssetLibrary &library,
    const blender::asset_system::AssetCatalogPath &path);

/**
 *
 */
AssetLibraryReference user_library_to_library_ref(const bUserAssetLibrary &user_library);

/**
 *
 */
const bUserAssetLibrary *library_ref_to_user_library(const AssetLibraryReference &library_ref);

/**
 *
 */
const bUserAssetLibrary *get_asset_library_from_prop(PointerRNA &ptr);

/**
 *
 */
void visit_library_catalogs_catalog_for_search(
    const Main &bmain,
    const bUserAssetLibrary &user_library,
    const StringRef edit_text,
    const FunctionRef<void(StringPropertySearchVisitParams)> visit_fn);

/**
 *
 */
void visit_library_prop_catalogs_catalog_for_search_fn(
    const bContext *C,
    PointerRNA *ptr,
    PropertyRNA * /*prop*/,
    const char *edit_text,
    FunctionRef<void(StringPropertySearchVisitParams)> visit_fn);

/**
 *
 */
void refresh_asset_library(const bContext *C, const AssetLibraryReference &library_ref);
void refresh_asset_library(const bContext *C, const bUserAssetLibrary &user_library);

/**
 *
 */
void show_catalog_in_asset_shelf(const bContext &C, const StringRefNull catalog_path);

}  // namespace blender::ed::asset
