/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include <string>

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BKE_preview_image.hh"

#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "ED_asset.hh"

namespace blender::ed::asset {

std::string asset_tooltip(const asset_system::AssetRepresentation &asset, const bool include_name)
{
  std::string complete_string;

  if (include_name) {
    complete_string += asset.get_name();
  }

  const AssetMetaData &meta_data = asset.get_metadata();
  if (meta_data.description) {
    complete_string += '\n';
    complete_string += meta_data.description;
  }
  return complete_string;
}

BIFIconID asset_preview_icon_id(const asset_system::AssetRepresentation &asset)
{
  if (const PreviewImage *preview = asset.preview_storage()) {
    return preview->runtime->icon_id;
  }
  return ICON_NONE;
}

BIFIconID asset_preview_or_icon(const asset_system::AssetRepresentation &asset)
{
  const PreviewImage *preview = asset.preview_storage();

  if (preview && !BKE_previewimg_is_finished(preview, ICON_SIZE_PREVIEW)) {
    /* Loading icon. */
    return ICON_TEMP;
  }

  if (preview && preview->runtime->icon_id) {
    return preview->runtime->icon_id;
  }

  /* ID type icon. */
  return UI_icon_from_idcode(asset.get_id_type());
}

}  // namespace blender::ed::asset
