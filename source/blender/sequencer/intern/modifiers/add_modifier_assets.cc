/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BKE_screen.hh"

#include "ED_asset.hh"
#include "ED_asset_menu_utils.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"

#include "WM_api.hh"

namespace blender::seq {

static MenuType modifier_add_unassigned_assets_menu_type()
{
  MenuType type{};
  STRNCPY_UTF8(type.idname, "SEQUENCER_MT_add_modifier_unassigned_assets");
  type.draw = unassigned_assets_draw;
  type.listener = ed::asset::list::asset_reading_region_listen_fn;
  type.description = N_(
      "Modifier node group assets not assigned to a catalog.\n"
      "Catalogs can be assigned in the Asset Browser");
  return type;
}

static MenuType modifier_add_catalog_assets_menu_type()
{
  MenuType type{};
  STRNCPY_UTF8(type.idname, "SEQUENCER_MT_add_modifier_catalog_assets");
  type.draw = catalog_assets_draw;
  type.listener = ed::asset::list::asset_reading_region_listen_fn;
  type.flag = MenuTypeFlag::ContextDependent;
  return type;
}

static MenuType modifier_add_root_catalogs_menu_type()
{
  MenuType type{};
  STRNCPY_UTF8(type.idname, "SEQUENCER_MT_modifier_add_root_catalogs");
  type.draw = root_catalogs_draw;
  type.listener = ed::asset::list::asset_reading_region_listen_fn;
  type.flag = MenuTypeFlag::ContextDependent;
  return type;
}

void object_modifier_add_asset_register()
{
  WM_menutype_add(MEM_new<MenuType>(__func__, modifier_add_catalog_assets_menu_type()));
  WM_menutype_add(MEM_new<MenuType>(__func__, modifier_add_unassigned_assets_menu_type()));
  WM_menutype_add(MEM_new<MenuType>(__func__, modifier_add_root_catalogs_menu_type()));
  WM_operatortype_append(OBJECT_OT_modifier_add_node_group);
}

}  // namespace blender::seq
