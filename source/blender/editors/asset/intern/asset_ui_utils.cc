/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 */

#include <algorithm>
#include <string>

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "BKE_context.hh"
#include "BKE_preferences.h"
#include "BKE_preview_image.hh"

#include "DNA_ID.h"
#include "DNA_asset_types.h"
#include "DNA_listbase.h"
#include "DNA_space_enums.h"

#include "BKE_idprop.hh"

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_string_utils.hh"
#include "MEM_guardedalloc.h"

#include "BLT_translation.hh"

#include "UI_interface_c.hh"
#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "DNA_userdef_types.h"

#include "RNA_access.hh"

#include "RNA_prototypes.hh"
#include "WM_api.hh"
#include "WM_keymap.hh"
#include "WM_types.hh"
#include "wm_event_types.hh"

#include "ED_asset.hh"

namespace blender::ed::asset {
/**
 * Helper function to clean up operator properties and template resources.
 */
static void cleanup_operator_resources(IDProperty *search_properties,
                                       PointerRNA *op_props_ptr_template)
{
  if (search_properties) {
    IDP_FreeProperty(search_properties);
  }
  if (op_props_ptr_template) {
    WM_operator_properties_free(op_props_ptr_template);
    MEM_delete(op_props_ptr_template);
  }
}
/**
 * Custom property comparison function that ignores asset_library_type differences
 * for brush asset activation operators. This allows matching user library brushes
 * (library_type=100) with system keymap entries (library_type=1).
 */
static std::string normalize_asset_path(const std::string &path)
{
  std::string normalized = path;
  /* Convert backslashes to forward slashes for consistent path matching */
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  return normalized;
}
static bool compare_brush_asset_properties(const IDProperty *search_props,
                                           const IDProperty *kmi_props)
{
  if (!search_props || !kmi_props || search_props->type != IDP_GROUP ||
      kmi_props->type != IDP_GROUP)
  {
    return false;
  }

  // Compare relative_asset_identifier (most important)
  const IDProperty *search_id = IDP_GetPropertyFromGroup(search_props,
                                                         "relative_asset_identifier");
  const IDProperty *kmi_id = IDP_GetPropertyFromGroup(kmi_props, "relative_asset_identifier");

  if (!search_id || !kmi_id) {
    return false;
  }

  // Normalize and compare paths
  if (search_id->type == IDP_STRING && kmi_id->type == IDP_STRING) {
    const char *search_str = static_cast<const char *>(search_id->data.pointer);
    const char *kmi_str = static_cast<const char *>(kmi_id->data.pointer);

    if (search_str && kmi_str) {
      return normalize_asset_path(search_str) == normalize_asset_path(kmi_str);
    }
  }

  // Fallback to direct comparison
  if (!IDP_EqualsProperties_ex(search_id, kmi_id, true)) {
    return false;
  }

  // Compare asset_library_identifier - treat empty string as equivalent to missing
  const IDProperty *search_lib_id = IDP_GetPropertyFromGroup(search_props,
                                                             "asset_library_identifier");
  const IDProperty *kmi_lib_id = IDP_GetPropertyFromGroup(kmi_props, "asset_library_identifier");

  if (search_lib_id && kmi_lib_id) {
    return IDP_EqualsProperties_ex(search_lib_id, kmi_lib_id, true);
  }

  // If only one has library identifier, check if it's empty
  if (search_lib_id || kmi_lib_id) {
    const IDProperty *existing_prop = search_lib_id ? search_lib_id : kmi_lib_id;
    if (existing_prop->type == IDP_STRING && existing_prop->data.pointer) {
      const char *str_val = static_cast<const char *>(existing_prop->data.pointer);
      return !str_val || strlen(str_val) == 0;
    }
  }

  return true;
}
/**
 * Create operator properties for keymap search based on asset weak reference.
 */
static IDProperty *create_operator_properties(const AssetWeakReference &weak_ref,
                                              StructRNA *op_props_type,
                                              const char *op_name)
{
  /* Create a new property group with the correct name. */
  IDProperty *search_properties = IDP_New(IDP_GROUP, nullptr, "wmOpItemProp");
  /* Associate the operator's RNA type with this new group. */
  PointerRNA search_op_props_ptr = RNA_pointer_create_discrete(
      nullptr, op_props_type, search_properties);
  /* Handle different operator types with specific property requirements */
  if (STREQ(op_name, "WM_OT_tool_set_by_id")) {
    /* For tool operators, use the asset name as the tool identifier */
    const std::string normalized_path = normalize_asset_path(weak_ref.relative_asset_identifier);
    RNA_string_set(&search_op_props_ptr, "name", normalized_path.c_str());
  }
  else if (STREQ(op_name, "PAINT_OT_brush_select")) {
    /* For brush select operators, use brush name or index */
    const std::string normalized_path = normalize_asset_path(weak_ref.relative_asset_identifier);
    /* Try to set brush name if the property exists */
    if (RNA_struct_find_property(&search_op_props_ptr, "brush")) {
      RNA_string_set(&search_op_props_ptr, "brush", normalized_path.c_str());
    }
  }
  else {
    /* For asset-based operators (like BRUSH_OT_asset_activate) */
    /* Use actual library type for proper matching, comparison function will handle differences */
    int keymap_library_type = weak_ref.asset_library_type;
    RNA_enum_set(&search_op_props_ptr, "asset_library_type", keymap_library_type);
    /* Normalize path for consistent matching */
    const std::string normalized_path = normalize_asset_path(weak_ref.relative_asset_identifier);
    RNA_string_set(&search_op_props_ptr, "relative_asset_identifier", normalized_path.c_str());
    /* Always set asset_library_identifier for consistent property matching */
    if (weak_ref.asset_library_type == ASSET_LIBRARY_CUSTOM && weak_ref.asset_library_identifier) {
      RNA_string_set(
          &search_op_props_ptr, "asset_library_identifier", weak_ref.asset_library_identifier);
    }
    else {
      /* For essentials library or keymap matching, use empty string as identifier */
      RNA_string_set(&search_op_props_ptr, "asset_library_identifier", "");
    }
  }
  return search_properties;
}
/**
 * Search for brush operator in keymap using custom property comparison.
 */
static std::string search_keymap_for_brush_operator(wmWindowManager *wm,
                                                    const char *op_name,
                                                    IDProperty *search_properties)
{
  LISTBASE_FOREACH (wmKeyMap *, keymap, &wm->runtime->userconf->keymaps) {
    LISTBASE_FOREACH (wmKeyMapItem *, kmi, &keymap->items) {
      if (STREQ(kmi->idname, op_name) && kmi->ptr && kmi->ptr->data) {
        if (compare_brush_asset_properties(search_properties, (IDProperty *)kmi->ptr->data)) {
          std::optional<std::string> hotkey_opt = WM_keymap_item_to_string(kmi, false);
          if (hotkey_opt.has_value()) {
            return hotkey_opt.value();
          }
        }
      }
    }
  }
  return "";
}
/**
 * Create fallback operator properties with system library type for better keymap compatibility.
 * This helps match user library brushes with system keymap entries.
 */
static IDProperty *create_fallback_operator_properties(const AssetWeakReference &weak_ref,
                                                       StructRNA *op_props_type,
                                                       const char *op_name)
{
  /* Create fallback properties with system library type */
  AssetWeakReference fallback_ref = weak_ref;
  fallback_ref.asset_library_type = ASSET_LIBRARY_ESSENTIALS;  // Use system library type

  return create_operator_properties(fallback_ref, op_props_type, op_name);
}
/**
 * Find hotkey for specific operator with given properties.
 */
static std::string find_hotkey_for_operator(const bContext *C,
                                            const char *op_name,
                                            const AssetWeakReference &weak_ref,
                                            bool is_strict)
{
  /* Get the RNA type for the operator's properties. */
  PointerRNA *op_props_ptr_template = nullptr;
  IDProperty *properties_template = nullptr;
  WM_operator_properties_alloc(&op_props_ptr_template, &properties_template, op_name);

  if (!op_props_ptr_template) {
    return "";
  }

  StructRNA *op_props_type = op_props_ptr_template->type;
  /* Create properties for keymap search */
  IDProperty *search_properties = create_operator_properties(weak_ref, op_props_type, op_name);
  /* For BRUSH_OT_asset_activate, use custom property comparison */
  if (STREQ(op_name, "BRUSH_OT_asset_activate")) {
    wmWindowManager *wm = CTX_wm_manager(C);
    if (wm) {
      // Try with original properties
      std::string result = search_keymap_for_brush_operator(wm, op_name, search_properties);
      if (!result.empty()) {
        cleanup_operator_resources(search_properties, op_props_ptr_template);
        return result;
      }

      // Fallback: Try with system library type for better compatibility
      if (weak_ref.asset_library_type != ASSET_LIBRARY_ESSENTIALS) {
        IDProperty *fallback_properties = create_fallback_operator_properties(
            weak_ref, op_props_type, op_name);
        result = search_keymap_for_brush_operator(wm, op_name, fallback_properties);
        IDP_FreeProperty(fallback_properties);

        if (!result.empty()) {
          cleanup_operator_resources(search_properties, op_props_ptr_template);
          return result;
        }
      }
    }
  }
  else {
    /* Use standard search for other operators */
    std::optional<std::string> hotkey = WM_key_event_operator_string(
        C, op_name, blender::wm::OpCallContext::InvokeDefault, search_properties, is_strict);

    cleanup_operator_resources(search_properties, op_props_ptr_template);
    return hotkey.value_or("");
  }
  cleanup_operator_resources(search_properties, op_props_ptr_template);
  return "";
}
/**
 * Search for hotkeys in specific keymap contexts.
 */
static std::string find_hotkey_in_context(const bContext *C,
                                          const char *op_name,
                                          const AssetWeakReference &weak_ref,
                                          const char *keymap_name)
{

  wmWindowManager *wm = CTX_wm_manager(C);
  if (!wm) {
    return "";
  }
  /* First try to find the keymap in default configuration */
  wmKeyMap *default_keymap = nullptr;
  if (wm->runtime->defaultconf) {
    default_keymap = WM_keymap_list_find(
        &wm->runtime->defaultconf->keymaps, keymap_name, SPACE_EMPTY, RGN_TYPE_WINDOW);
  }

  /* Get the active keymap (includes user modifications) */
  wmKeyMap *keymap = WM_keymap_active(wm, default_keymap);
  if (!keymap) {
    /* Fallback: try to find directly in user configuration */
    keymap = WM_keymap_find_all(wm, keymap_name, SPACE_EMPTY, RGN_TYPE_WINDOW);
    if (!keymap) {
      return "";
    }
  }
  /* Get operator properties template */
  PointerRNA *op_props_ptr_template = nullptr;
  IDProperty *properties_template = nullptr;
  WM_operator_properties_alloc(&op_props_ptr_template, &properties_template, op_name);

  if (!op_props_ptr_template) {
    return "";
  }

  /* Create search properties */
  IDProperty *search_properties = create_operator_properties(
      weak_ref, op_props_ptr_template->type, op_name);

  std::string result = "";

  /* Use standard search for all operators - this uses the same logic as
   * WM_key_event_operator_from_keymap */
  wmKeyMapItem *kmi = WM_key_event_operator_from_keymap(
      keymap, op_name, search_properties, EVT_TYPE_MASK_ALL, 0);

  if (kmi) {
    std::optional<std::string> hotkey_opt = WM_keymap_item_to_string(kmi, false);
    if (hotkey_opt.has_value()) {
      result = hotkey_opt.value();
    }
  }

  /* If standard search failed for BRUSH_OT_asset_activate, try custom comparison as fallback */
  if (result.empty() && STREQ(op_name, "BRUSH_OT_asset_activate")) {
    LISTBASE_FOREACH (wmKeyMapItem *, kmi_fallback, &keymap->items) {
      if (STREQ(kmi_fallback->idname, op_name) && kmi_fallback->ptr && kmi_fallback->ptr->data) {
        if (compare_brush_asset_properties(search_properties,
                                           (IDProperty *)kmi_fallback->ptr->data))
        {
          std::optional<std::string> hotkey_opt = WM_keymap_item_to_string(kmi_fallback, false);
          if (hotkey_opt.has_value()) {
            result = hotkey_opt.value();
            break;
          }
        }
      }
    }
  }

  /* Clean up */
  cleanup_operator_resources(search_properties, op_props_ptr_template);

  return result;
}
/**
 * Find hotkey through alternative operators and search methods.
 */
static std::string find_alternative_hotkeys(const bContext *C, const AssetWeakReference &weak_ref)
{
  /* Try PAINT_OT_brush_select operator */
  std::string hotkey = find_hotkey_for_operator(C, "PAINT_OT_brush_select", weak_ref, false);

  if (!hotkey.empty()) {
    return hotkey;
  }
  /* Try WM_OT_tool_set_by_id operator */
  hotkey = find_hotkey_for_operator(C, "WM_OT_tool_set_by_id", weak_ref, false);

  if (!hotkey.empty()) {
    return hotkey;
  }
  /* Try searching in specific sculpting contexts */
  const char *sculpt_keymaps[] = {"Sculpt", "3D View", "Window", nullptr};

  for (int i = 0; sculpt_keymaps[i]; i++) {
    hotkey = find_hotkey_in_context(C, "WM_OT_tool_set_by_id", weak_ref, sculpt_keymaps[i]);
    if (!hotkey.empty()) {
      return hotkey;
    }
  }
  /* Try searching by brush name without path extension */
  const std::string asset_name = normalize_asset_path(weak_ref.relative_asset_identifier);
  const size_t dot_pos = asset_name.find_last_of('.');
  if (dot_pos != std::string::npos) {
    const std::string brush_name = asset_name.substr(0, dot_pos);

    /* Create a modified weak reference with just the brush name */
    AssetWeakReference name_ref;
    name_ref.asset_library_type = weak_ref.asset_library_type;
    name_ref.asset_library_identifier = weak_ref.asset_library_identifier ?
                                            BLI_strdup(weak_ref.asset_library_identifier) :
                                            nullptr;
    name_ref.relative_asset_identifier = BLI_strdup(brush_name.c_str());

    /* Try tool operator with brush name in different contexts */
    for (int i = 0; sculpt_keymaps[i]; i++) {
      hotkey = find_hotkey_in_context(C, "WM_OT_tool_set_by_id", name_ref, sculpt_keymaps[i]);
      if (!hotkey.empty()) {
        return hotkey;
      }
    }
  }

  return "";
}
/**
 * Get hotkey string for brush asset activation.
 * Returns empty string if no hotkey is found.
 */
static std::string get_brush_asset_hotkey(const bContext *C,
                                          const asset_system::AssetRepresentation &asset)
{
  /* Only check for brush assets. */
  if (asset.get_id_type() != ID_BR) {
    return "";
  }
  const AssetWeakReference weak_ref = asset.make_weak_reference();

  /* Try primary operator with strict search */
  std::string hotkey = find_hotkey_for_operator(C, "BRUSH_OT_asset_activate", weak_ref, true);

  if (!hotkey.empty()) {
    return hotkey;
  }
  /* Fallback to non-strict search */
  hotkey = find_hotkey_for_operator(C, "BRUSH_OT_asset_activate", weak_ref, false);

  if (!hotkey.empty()) {
    return hotkey;
  }
  /* For custom library assets (like user brushes), also search in Sculpt keymap */
  if (weak_ref.asset_library_type == ASSET_LIBRARY_CUSTOM) {
    hotkey = find_hotkey_in_context(C, "BRUSH_OT_asset_activate", weak_ref, "Sculpt");

    if (!hotkey.empty()) {
      return hotkey;
    }
  }
  /* Try alternative operators */
  hotkey = find_alternative_hotkeys(C, weak_ref);

  return hotkey;
}

void asset_tooltip(const bContext *C,
                   const asset_system::AssetRepresentation &asset,
                   uiTooltipData &tip,
                   const bool include_name)
{
  if (include_name) {
    UI_tooltip_text_field_add(tip, asset.get_name(), {}, UI_TIP_STYLE_HEADER, UI_TIP_LC_MAIN);

    UI_tooltip_text_field_add(tip, {}, {}, UI_TIP_STYLE_SPACER, UI_TIP_LC_NORMAL, false);
    /* Add hotkey information immediately after name for brush assets */
    if (C && asset.get_id_type() == ID_BR) {
      const std::string hotkey = get_brush_asset_hotkey(C, asset);
      if (!hotkey.empty()) {
        UI_tooltip_multicolor_text_field_add(tip,
                                             "Shortcut: ",
                                             hotkey.c_str(),
                                             UI_TIP_STYLE_NORMAL,
                                             UI_TIP_LC_VALUE,
                                             UI_TIP_LC_ACTIVE,
                                             true);
      }
      else {
        UI_tooltip_text_field_add(
            tip, "No Shortcut Assigned", {}, UI_TIP_STYLE_NORMAL, UI_TIP_LC_VALUE, false);
      }
    }

    UI_tooltip_text_field_add(tip, {}, {}, UI_TIP_STYLE_SPACER, UI_TIP_LC_NORMAL, false);
  }

  const AssetMetaData &meta_data = asset.get_metadata();
  if (meta_data.description) {
    UI_tooltip_text_field_add(tip, meta_data.description, {}, UI_TIP_STYLE_HEADER, UI_TIP_LC_MAIN);
  }

  switch (asset.owner_asset_library().library_type()) {
    case ASSET_LIBRARY_CUSTOM: {
      UI_tooltip_text_field_add(tip, {}, {}, UI_TIP_STYLE_SPACER, UI_TIP_LC_NORMAL, false);

      const std::string full_blend_path = asset.full_library_path();

      char dir[FILE_MAX], file[FILE_MAX];
      BLI_path_split_dir_file(full_blend_path.c_str(), dir, sizeof(dir), file, sizeof(file));

      if (file[0]) {
        UI_tooltip_text_field_add(tip, file, {}, UI_TIP_STYLE_NORMAL, UI_TIP_LC_MAIN);
      }
      if (dir[0]) {
        UI_tooltip_text_field_add(tip, dir, {}, UI_TIP_STYLE_NORMAL, UI_TIP_LC_MAIN);
      }
      break;
    }
    case ASSET_LIBRARY_LOCAL:
      UI_tooltip_text_field_add(tip, {}, {}, UI_TIP_STYLE_SPACER, UI_TIP_LC_NORMAL, false);
      UI_tooltip_text_field_add(
          tip, TIP_("Asset Library: Current File"), {}, UI_TIP_STYLE_NORMAL, UI_TIP_LC_VALUE);
      break;
    case ASSET_LIBRARY_ESSENTIALS:
      UI_tooltip_text_field_add(tip, {}, {}, UI_TIP_STYLE_SPACER, UI_TIP_LC_NORMAL, false);
      UI_tooltip_text_field_add(
          tip, TIP_("Asset Library: Essentials"), {}, UI_TIP_STYLE_NORMAL, UI_TIP_LC_VALUE);
      break;
    default:
      /* Intentionally empty. */
      break;
  }
}

BIFIconID asset_preview_icon_id(const asset_system::AssetRepresentation &asset)
{
  if (const PreviewImage *preview = asset.get_preview()) {
    if (!BKE_previewimg_is_invalid(preview)) {
      return preview->runtime->icon_id;
    }
  }

  return ICON_NONE;
}

BIFIconID asset_preview_or_icon(const asset_system::AssetRepresentation &asset)
{
  const BIFIconID preview_icon = asset_preview_icon_id(asset);
  if (preview_icon != ICON_NONE) {
    return preview_icon;
  }

  /* Preview image not found or invalid. Use type icon. */
  return UI_icon_from_idcode(asset.get_id_type());
}

const bUserAssetLibrary *get_asset_library_from_opptr(PointerRNA &ptr)
{
  const int enum_value = RNA_enum_get(&ptr, "asset_library_reference");
  const AssetLibraryReference lib_ref = asset::library_reference_from_enum_value(enum_value);
  return BKE_preferences_asset_library_find_index(&U, lib_ref.custom_library_index);
}

AssetLibraryReference get_asset_library_ref_from_opptr(PointerRNA &ptr)
{
  const int enum_value = RNA_enum_get(&ptr, "asset_library_reference");
  return asset::library_reference_from_enum_value(enum_value);
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

}  // namespace blender::ed::asset
