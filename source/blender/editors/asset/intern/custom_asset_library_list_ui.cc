/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spuserpref
 */

#include "BKE_global.hh"

#include "BLT_translation.hh"

#include "DNA_screen_types.h"

#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "ED_asset_library_ui.hh"

namespace blender {

void draw_active_library_settings(ui::Layout &layout, const AnyAssetLibraryDefinition &library)
{
  if (library.type == ASSET_LIBRARY_ESSENTIALS) {
    PointerRNA prefs_ptr = RNA_pointer_create_discrete(nullptr, RNA_PreferencesAssetLibraries, &U);

    ui::Layout &row = layout.row(false);
    row.active_set((G.f & G_FLAG_INTERNET_ALLOW) != 0);
    row.prop(&prefs_ptr,
             "use_online_essentials",
             UI_ITEM_NONE,
             IFACE_("Include Online Essentials"),
             ICON_NONE);
  }

  if (library.user_library) {
    PointerRNA library_ptr = RNA_pointer_create_discrete(
        nullptr, RNA_UserAssetLibrary, library.user_library);

    if (library.user_library->flag & ASSET_LIBRARY_USE_REMOTE_URL) {
      if (USER_EXPERIMENTAL_TEST(&U, use_remote_asset_libraries)) {
        ui::Layout &row = layout.row(false);
        row.red_alert_set(!library.user_library->remote_url[0]);
        row.prop(&library_ptr,
                 RNA_struct_find_property(&library_ptr, "remote_url"),
                 RNA_NO_INDEX,
                 0,
                 UI_ITEM_NONE,
                 "",
                 ICON_INTERNET,
                 IFACE_("Repository URL"));
      }
      layout.prop(&library_ptr, "import_method", UI_ITEM_NONE, IFACE_("Import Method"), ICON_NONE);
    }
    else {
      layout.prop(&library_ptr, "path", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      layout.prop(&library_ptr, "import_method", UI_ITEM_NONE, IFACE_("Import Method"), ICON_NONE);
      layout.prop(&library_ptr, "use_relative_path", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }
  }
}

}  // namespace blender
