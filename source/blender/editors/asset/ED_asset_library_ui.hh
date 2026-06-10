/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spuserpref
 */

#pragma once

#include "BLT_translation.hh"

#include "RNA_access.hh"
#include "RNA_enum_types.hh"
#include "RNA_prototypes.hh"

#include "UI_tree_view.hh"

namespace blender {

struct AnyAssetLibraryDefinition {
  eAssetLibraryType type;
  bUserAssetLibrary *user_library;
};

struct AssetLibraryListItemCommon : public ui::AbstractTreeViewItem {
  AnyAssetLibraryDefinition library;
  int index_in_list = 0;

  AssetLibraryListItemCommon(const AnyAssetLibraryDefinition &library, const int index_in_list)
      : library(library), index_in_list(index_in_list)
  {

    if (library.user_library) {
      label_ = library.user_library->name;
    }
    else {
      const char *name_cstr;
      RNA_enum_name_gettexted(
          rna_enum_asset_library_type_items, library.type, BLT_I18NCONTEXT_DEFAULT, &name_cstr);
      label_ = name_cstr;
    }
  }

  void build_row(ui::Layout &row) override
  {
    row.label("Implement your own list row drawing code!", ICON_ERROR);
  }

  bool supports_renaming() const override
  {
    return library.user_library != nullptr;
  }
  bool rename(const bContext &C, StringRefNull new_name) override
  {
    PointerRNA ptr = RNA_pointer_create_discrete(
        nullptr, RNA_UserAssetLibrary, library.user_library);
    PropertyRNA *prop = RNA_struct_find_property(&ptr, "name");
    RNA_property_string_set(&ptr, prop, new_name.c_str());
    RNA_property_update(&const_cast<bContext &>(C), &ptr, prop);
    return true;
  }
};

template<typename AssetLibraryListItemType> struct AssetLibraryList : public ui::AbstractTreeView {
  Vector<AnyAssetLibraryDefinition> libraries;

  AssetLibraryList(const Vector<AnyAssetLibraryDefinition> libraries) : libraries(libraries) {};

  void build_tree() override
  {
    this->is_flat_ = true;

    int i = 0;
    for (const AnyAssetLibraryDefinition &library : libraries) {
      add_tree_item<AssetLibraryListItemType>(library, i++);
    }
  }
};

template<typename AssetLibraryListItemType>
void draw_library_list(const bContext &C,
                       ui::Layout &layout,
                       Vector<AnyAssetLibraryDefinition> &libraries)
{
  ui::Block *block = layout.block();

  ui::AbstractTreeView *tree_view = block_add_view(
      *block,
      "Asset Libraries Preferences",
      std::make_unique<AssetLibraryList<AssetLibraryListItemType>>(libraries));
  tree_view->set_default_rows(5);

  ui::TreeViewBuilder::build_tree_view(C, *tree_view, layout);
}

void draw_active_library_settings(ui::Layout &layout, const AnyAssetLibraryDefinition &library);

}  // namespace blender
