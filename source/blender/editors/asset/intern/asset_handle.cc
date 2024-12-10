/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include <string>

#include "AS_asset_representation.hh"

#include "BKE_preview_image.hh"

#include "DNA_space_types.h"

#include "DNA_space_types.h"

#include "ED_fileselect.hh"

#include "ED_asset_handle.hh"

namespace blender::ed::asset {

asset_system::AssetRepresentation *handle_get_representation(const AssetHandle *asset)
{
  return asset->file_data->asset;
}

int handle_get_preview_icon_id(const AssetHandle *asset_handle)
{
  const asset_system::AssetRepresentation *asset = handle_get_representation(asset_handle);
  if (const PreviewImage *preview = asset->preview_storage()) {
    return preview->runtime->icon_id;
  }
  return 0;
}

int handle_get_preview_or_type_icon_id(const AssetHandle *asset)
{
  const int preview_id = handle_get_preview_icon_id(asset);
  return preview_id ? preview_id : ED_file_icon(asset->file_data);
}

}  // namespace blender::ed::asset
