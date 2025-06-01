/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BLI_string_ref.hh"

#include "DNA_customdata_types.h"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"

#include "BLT_translation.hh"

#include "BKE_attribute.hh"
#include "BKE_grease_pencil.hh"

#include "NOD_geometry_nodes_log.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"
#include "UI_string_search.hh"

#include <fmt/format.h>

using blender::bke::greasepencil::LayerSearchInfo;

namespace blender::ui {

void grease_pencil_layer_search_add_items(const StringRef str,
                                          const Span<LayerSearchInfo> filtered_layer_infos,
                                          uiSearchItems &seach_items,
                                          const bool is_first)
{
  static LayerSearchInfo dummy_info;

  /* Any string may be valid, so add the current search string along with the hints. */
  if (!str.is_empty()) {
    bool contained = false;
    for (const LayerSearchInfo &item : filtered_layer_infos) {
      if (item.name == str) {
        contained = true;
      }
    }
    if (!contained) {
      dummy_info.name = str;
      UI_search_item_add(&seach_items, str, &dummy_info, ICON_NONE, 0, 0);
    }
  }

  if (str.is_empty() && !is_first) {
    /* Allow clearing the text field when the string is empty, but not on the first pass,
     * or opening a layer name field for the first time would show this search item. */
    dummy_info.name = str;
    UI_search_item_add(&seach_items, str, &dummy_info, ICON_X, 0, 0);
  }

  /* Don't filter when the menu is first opened, but still run the search
   * so the items are in the same order they will appear in while searching. */
  const StringRef string = is_first ? "" : str;

  ui::string_search::StringSearch<const LayerSearchInfo> search;
  for (const LayerSearchInfo &item : filtered_layer_infos) {
    search.add(StringRef(item.name), &item);
  }

  const Vector<const LayerSearchInfo *> filtered_items = search.query(string);
  for (const LayerSearchInfo *item : filtered_items) {
    if (!UI_search_item_add(&seach_items,
                            item->name,
                            (void *)item,
                            item->is_group ? ICON_GREASEPENCIL_LAYER_GROUP :
                                             ICON_OUTLINER_DATA_GP_LAYER,
                            UI_BUT_HAS_SEP_CHAR,
                            0))
    {
      break;
    }
  }
}

}  // namespace blender::ui
