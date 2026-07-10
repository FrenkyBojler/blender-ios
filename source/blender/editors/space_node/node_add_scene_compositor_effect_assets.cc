/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BLI_listbase.hh"
#include "BLI_string_utf8.hh"

#include "BKE_asset.hh"
#include "BKE_compositor.hh"
#include "BKE_context.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "ED_asset.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_object.hh"

#include "NOD_composite.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"

namespace blender::ed::space_node {

static bool all_loading_finished()
{
  AssetLibraryReference all_library_ref = asset_system::all_library_reference();
  return asset::list::is_loaded(&all_library_ref);
}

static asset::AssetItemTree build_catalog_tree(const bContext &C)
{
  asset::AssetFilterSettings type_filter{};
  type_filter.id_types = FILTER_ID_NT;
  auto meta_data_filter = [&](const AssetMetaData &meta_data) {
    const IDProperty *tree_type = BKE_asset_metadata_idprop_find(&meta_data, "type");
    if (tree_type == nullptr || IDP_int_get(tree_type) != NTREE_COMPOSIT) {
      return false;
    }
    const IDProperty *traits_flag = BKE_asset_metadata_idprop_find(
        &meta_data, "compositor_node_asset_traits_flag");
    if (traits_flag == nullptr || !(IDP_int_get(traits_flag) & COMPOSIT_NODE_ASSET_SCENE_EFFECT)) {
      return false;
    }
    return true;
  };
  const AssetLibraryReference library = asset_system::all_library_reference();
  asset_system::all_library_reload_catalogs_if_dirty();
  return asset::build_filtered_all_catalog_tree(
      library, C, type_filter, meta_data_filter, ntreeType_Composite->asset_catalog_path_prefix);
}

static asset::AssetItemTree *get_static_item_tree()
{
  static asset::AssetItemTree tree;
  return &tree;
}

static void catalog_assets_draw(const bContext *C, Menu *menu)
{
  asset::AssetItemTree &tree = *get_static_item_tree();

  const std::optional<StringRefNull> menu_path = CTX_data_string_get(C, "asset_catalog_path");
  if (!menu_path) {
    return;
  }
  const Span<asset_system::AssetRepresentation *> assets = tree.assets_per_path.lookup(
      menu_path->data());
  const asset_system::AssetCatalogTreeItem *catalog_item = tree.catalogs.find_item(
      menu_path->data());
  BLI_assert(catalog_item != nullptr);

  if (assets.is_empty() && !catalog_item->has_children()) {
    return;
  }

  ui::Layout &layout = *menu->layout;

  bool first = true;
  const auto ensure_separator = [&]() {
    if (first) {
      layout.separator();
      first = false;
    }
  };

  wmOperatorType *ot = WM_operatortype_find("SCENE_OT_add_compositor_effect_node_group_asset",
                                            true);
  for (const asset_system::AssetRepresentation *asset : assets) {
    ensure_separator();
    asset::draw_asset_menu_item(asset, ot->idname, layout);
  }

  catalog_item->foreach_child([&](const asset_system::AssetCatalogTreeItem &item) {
    ensure_separator();
    asset::draw_menu_for_catalog(
        item, "NODE_MT_add_scene_compositor_effect_catalog_assets", layout);
  });
}

static bool unassigned_local_poll(const Main &bmain)
{
  for (const bNodeTree &group : bmain.nodetrees) {
    /* Assets are displayed in other menus, and non-local data-blocks aren't added to this menu. */
    if (ID_IS_ASSET(&group.id)) {
      continue;
    }
    if (!group.compositor_node_asset_traits ||
        !(group.compositor_node_asset_traits->flag & COMPOSIT_NODE_ASSET_SCENE_EFFECT))
    {
      continue;
    }
    return true;
  }
  return false;
}

static void unassigned_assets_draw(const bContext *C, Menu *menu)
{
  Main &bmain = *CTX_data_main(C);
  asset::AssetItemTree &tree = *get_static_item_tree();
  ui::Layout &layout = *menu->layout;
  wmOperatorType *ot = WM_operatortype_find("SCENE_OT_add_compositor_effect_node_group_asset",
                                            true);
  for (const asset_system::AssetRepresentation *asset : tree.unassigned_assets) {
    asset::draw_asset_menu_item(asset, ot->idname, layout);
  }

  bool first = true;
  bool add_separator = !tree.unassigned_assets.is_empty();
  for (const bNodeTree &group : bmain.nodetrees) {
    /* Assets are displayed in other menus, and non-local data-blocks aren't added to this menu. */
    if (ID_IS_ASSET(&group.id)) {
      continue;
    }
    if (!group.compositor_node_asset_traits ||
        !(group.compositor_node_asset_traits->flag & COMPOSIT_NODE_ASSET_SCENE_EFFECT))
    {
      continue;
    }

    if (add_separator) {
      layout.separator();
      add_separator = false;
    }
    if (first) {
      layout.label(IFACE_("Non-Assets"), ICON_NONE);
      first = false;
    }

    PointerRNA props_ptr = layout.op(
        ot, group.id.name + 2, ICON_NONE, wm::OpCallContext::InvokeDefault, UI_ITEM_NONE);
    WM_operator_properties_id_lookup_set_from_id(&props_ptr, &group.id);
  }
}

static void root_catalogs_draw(const bContext *C, Menu *menu)
{
  ui::Layout &layout = *menu->layout;

  const bool loading_finished = all_loading_finished();

  asset::AssetItemTree &tree = *get_static_item_tree();
  tree = build_catalog_tree(*C);
  if (!tree.catalogs.is_empty() || loading_finished) {
    layout.separator();

    if (!loading_finished) {
      layout.label(IFACE_("Loading Asset Libraries"), ICON_INFO);
    }

    tree.catalogs.foreach_root_item([&](const asset_system::AssetCatalogTreeItem &item) {
      asset::draw_menu_for_catalog(item, "SEQUENCER_MT_add_effect_catalog_assets", layout);
    });
  }

  if (!tree.unassigned_assets.is_empty() || unassigned_local_poll(*CTX_data_main(C))) {
    layout.separator();
    layout.menu("NODE_MT_add_scene_compositor_effect_unassigned_assets",
                IFACE_("Unassigned"),
                ICON_FILE_HIDDEN);
  }
}

static MenuType unassigned_assets_menu_type()
{
  MenuType type{};
  STRNCPY_UTF8(type.idname, "NODE_MT_add_scene_compositor_effect_unassigned_assets");
  type.draw = unassigned_assets_draw;
  type.listener = asset::list::asset_reading_region_listen_fn;
  type.description = N_(
      "Effect node group assets not assigned to a catalog.\n"
      "Catalogs can be assigned in the Asset Browser");
  return type;
}

static MenuType catalog_assets_menu_type()
{
  MenuType type{};
  STRNCPY_UTF8(type.idname, "NODE_MT_add_scene_compositor_effect_catalog_assets");
  type.draw = catalog_assets_draw;
  type.listener = asset::list::asset_reading_region_listen_fn;
  type.flag = MenuTypeFlag::ContextDependent;
  return type;
}

static MenuType root_catalogs_menu_type()
{
  MenuType type{};
  STRNCPY_UTF8(type.idname, "NODE_MT_add_scene_compositor_effect_root_catalogs");
  type.draw = root_catalogs_draw;
  type.listener = asset::list::asset_reading_region_listen_fn;
  type.flag = MenuTypeFlag::ContextDependent;
  return type;
}

void node_scene_compositor_effect_add_asset_register()
{
  WM_menutype_add(MEM_new<MenuType>(__func__, catalog_assets_menu_type()));
  WM_menutype_add(MEM_new<MenuType>(__func__, unassigned_assets_menu_type()));
  WM_menutype_add(MEM_new<MenuType>(__func__, root_catalogs_menu_type()));
}

}  // namespace blender::ed::space_node
