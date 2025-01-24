/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_asset.hh"
#include "BKE_asset_edit.hh"
#include "BKE_context.hh"
#include "BKE_icons.h"
#include "BKE_lib_id.hh"
#include "BKE_preferences.h"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "WM_api.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "ED_asset.hh"
#include "ED_asset_library.hh"
#include "ED_asset_list.hh"
#include "ED_asset_mark_clear.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_asset_shelf.hh"
#include "ED_fileselect.hh"
#include "ED_screen.hh"

#include "UI_interface_icons.hh"
#include "UI_resources.hh"

#include "BLT_translation.hh"

#include "ANIM_action.hh"
#include "ANIM_action_iterators.hh"
#include "ANIM_keyframing.hh"
#include "ANIM_rna.hh"

#include "AS_asset_catalog.hh"
#include "AS_asset_catalog_tree.hh"
#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "anim_intern.hh"

namespace blender::ed::animrig {

static const EnumPropertyItem *rna_asset_library_reference_itemf(bContext * /*C*/,
                                                                 PointerRNA * /*ptr*/,
                                                                 PropertyRNA * /*prop*/,
                                                                 bool *r_free)
{
  EnumPropertyItem *items;
  int totitem = 0;
  EnumPropertyItem tmp;

  tmp = {
      ASSET_LIBRARY_LOCAL, "LOCAL", 0, "Current File", "Save the pose asset to the current file"};
  RNA_enum_item_add(&items, &totitem, &tmp);
  /* Because `ASSET_LIBRARY_LOCAL` will always be created. */
  *r_free = true;

  int i;
  LISTBASE_FOREACH_INDEX (bUserAssetLibrary *, user_library, &U.asset_libraries, i) {
    /* Note that the path itself isn't checked for validity here. If an invalid library path is
     * used, the Asset Browser can give a nice hint on what's wrong. */
    const bool is_valid = (user_library->name[0] && user_library->dirpath[0]);
    if (!is_valid) {
      continue;
    }

    AssetLibraryReference library_reference;
    library_reference.type = ASSET_LIBRARY_CUSTOM;
    library_reference.custom_library_index = i;

    const int enum_value = blender::ed::asset::library_reference_to_enum_value(&library_reference);
    /* Use library path as description, it's a nice hint for users. */
    tmp = {enum_value, user_library->name, ICON_NONE, user_library->name, user_library->dirpath};
    RNA_enum_item_add(&items, &totitem, &tmp);
  }

  RNA_enum_item_end(&items, &totitem);

  BLI_assert(items != nullptr);
  return items;
}

static blender::animrig::Action &extract_pose(Main &bmain,
                                              const blender::Span<Object *> pose_objects)
{
  /* This currently only looks at the pose and not other things that could go onto different
   * slots on the same action. */

  using namespace blender::animrig;
  Action &action = action_add(bmain, "pose_create");
  Layer &layer = action.layer_add("pose");
  Strip &strip = layer.strip_add(action, Strip::Type::Keyframe);
  StripKeyframeData &strip_data = strip.data<StripKeyframeData>(action);
  const KeyframeSettings key_settings = {BEZT_KEYTYPE_KEYFRAME, HD_AUTO, BEZT_IPO_BEZ};

  for (Object *pose_object : pose_objects) {
    BLI_assert(pose_object->pose);
    Slot &slot = action.slot_add_for_id(pose_object->id);

    LISTBASE_FOREACH (bPoseChannel *, pose_bone, &pose_object->pose->chanbase) {
      if (!(pose_bone->bone->flag & BONE_SELECTED)) {
        continue;
      }
      PointerRNA bone_pointer = RNA_pointer_create(&pose_object->id, &RNA_PoseBone, pose_bone);
      Vector<RNAPath> rna_paths = construct_keyframing_rna_paths(&bone_pointer);
      for (const RNAPath &rna_path : rna_paths) {
        PointerRNA resolved_pointer;
        PropertyRNA *resolved_property;
        if (!RNA_path_resolve(
                &bone_pointer, rna_path.path.c_str(), &resolved_pointer, &resolved_property))
        {
          continue;
        }
        const Vector<float> values = blender::animrig::get_rna_values(&resolved_pointer,
                                                                      resolved_property);
        const std::optional<std::string> rna_path_id_to_prop = RNA_path_from_ID_to_property(
            &resolved_pointer, resolved_property);
        if (!rna_path_id_to_prop.has_value()) {
          continue;
        }
        for (const int i : values.index_range()) {
          strip_data.keyframe_insert(
              &bmain, slot, {rna_path_id_to_prop.value(), i}, {1, values[i]}, key_settings);
        }
      }
    }
  }
  return action;
}

/* Check that the newly created asset is visible SOMEWHERE in Blender. If not already visible,
 * open the asset shelf on the current 3D view. The reason for not always doing that is that it
 * might be annoying in case you have 2 3D viewports open, but you want the asset shelf on only one
 * of them, or you work out of the asset browser.*/
static void ensure_asset_ui_visible(bContext &C)
{
  ScrArea *current_area = CTX_wm_area(&C);
  if (!current_area || current_area->type->spaceid != SPACE_VIEW3D) {
    /* Opening the asset shelf will only work from the 3D viewport. */
    return;
  }

  wmWindowManager *wm = CTX_wm_manager(&C);
  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
    const bScreen *screen = WM_window_get_active_screen(win);
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      if (area->type->spaceid == SPACE_FILE) {
        SpaceFile *sfile = reinterpret_cast<SpaceFile *>(area->spacedata.first);
        if (sfile->browse_mode == FILE_BROWSE_MODE_ASSETS) {
          /* Asset Browser is open. */
          return;
        }
        continue;
      }
      const ARegion *shelf_region = BKE_area_find_region_type(area, RGN_TYPE_ASSET_SHELF);
      if (!shelf_region) {
        continue;
      }
      if (!(shelf_region->flag & RGN_FLAG_HIDDEN)) {
        /* A visible asset shelf was found. */
        return;
      }
    }
  }

  /* At this point, no asset shelf or asset browser was visible anywhere. */
  ARegion *shelf_region = BKE_area_find_region_type(current_area, RGN_TYPE_ASSET_SHELF);
  shelf_region->flag &= ~RGN_FLAG_HIDDEN;
  ED_region_visibility_change_update(&C, CTX_wm_area(&C), shelf_region);
}

static blender::Vector<Object *> get_selected_pose_objects(bContext *C)
{
  blender::Vector<PointerRNA> selected_objects;
  CTX_data_selected_objects(C, &selected_objects);

  blender::Vector<Object *> selected_pose_objects;
  for (const PointerRNA &ptr : selected_objects) {
    Object *object = reinterpret_cast<Object *>(ptr.owner_id);
    if (!object->pose) {
      continue;
    }
    selected_pose_objects.append(object);
  }

  Object *active_object = CTX_data_active_object(C);
  /* The active object may not be selected, it should be added because you can still switch to pose
   * mode. */
  if (active_object && active_object->pose && !selected_pose_objects.contains(active_object)) {
    selected_pose_objects.append(active_object);
  }
  return selected_pose_objects;
}

static int create_pose_asset_local(bContext *C,
                                   wmOperator *op,
                                   const StringRefNull name,
                                   const AssetLibraryReference lib_ref)
{
  blender::Vector<Object *> selected_pose_objects = get_selected_pose_objects(C);

  if (selected_pose_objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  /* Extract the pose into a new action. */
  blender::animrig::Action &pose_action = extract_pose(*bmain, selected_pose_objects);
  asset::mark_id(&pose_action.id);
  asset::generate_preview(C, &pose_action.id);
  BKE_id_rename(*bmain, pose_action.id, name);

  /* Add asset to catalog. */
  char catalog_path[MAX_NAME];
  RNA_string_get(op->ptr, "catalog_path", catalog_path);

  AssetMetaData &meta_data = *pose_action.id.asset_data;
  asset_system::AssetLibrary *library = AS_asset_library_load(bmain, lib_ref);
  /* I (christoph) don't know if a local library can fail to load. Just being defensive here */
  BLI_assert(library);
  if (catalog_path[0] && library) {
    const asset_system::AssetCatalog &catalog =
        blender::ed::asset::library_ensure_catalogs_in_path(*library, catalog_path);
    BKE_asset_metadata_catalog_id_set(&meta_data, catalog.catalog_id, catalog.simple_name.c_str());
  }

  ensure_asset_ui_visible(*C);
  blender::ed::asset::shelf::show_catalog_in_visible_shelves(*C, catalog_path);

  blender::ed::asset::refresh_asset_library(C, lib_ref);

  WM_main_add_notifier(NC_ASSET | ND_ASSET_LIST | NA_ADDED, nullptr);

  return OPERATOR_FINISHED;
}

static int create_pose_asset_user_library(bContext *C,
                                          wmOperator *op,
                                          const char name[MAX_NAME],
                                          const AssetLibraryReference lib_ref)
{
  BLI_assert(lib_ref.type == ASSET_LIBRARY_CUSTOM);
  Main *bmain = CTX_data_main(C);

  const bUserAssetLibrary *user_library = BKE_preferences_asset_library_find_index(
      &U, lib_ref.custom_library_index);
  if (!user_library) {
    return OPERATOR_CANCELLED;
  }

  asset_system::AssetLibrary *library = AS_asset_library_load(bmain, lib_ref);
  if (!library) {
    BKE_report(op->reports, RPT_ERROR, "Failed to load asset library");
    return OPERATOR_CANCELLED;
  }

  blender::Vector<Object *> selected_pose_objects = get_selected_pose_objects(C);

  if (selected_pose_objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  /* Temporary action in current main that will be exported and later deleted. */
  blender::animrig::Action &pose_action = extract_pose(*bmain, selected_pose_objects);
  asset::mark_id(&pose_action.id);
  asset::generate_preview(C, &pose_action.id);

  /* Add asset to catalog. */
  char catalog_path[MAX_NAME];
  RNA_string_get(op->ptr, "catalog_path", catalog_path);

  AssetMetaData &meta_data = *pose_action.id.asset_data;
  if (catalog_path[0]) {
    const asset_system::AssetCatalog &catalog =
        blender::ed::asset::library_ensure_catalogs_in_path(*library, catalog_path);
    BKE_asset_metadata_catalog_id_set(&meta_data, catalog.catalog_id, catalog.simple_name.c_str());
  }

  AssetWeakReference pose_asset_reference;
  const std::optional<std::string> final_full_asset_filepath = bke::asset_edit_id_save_as(
      *bmain, pose_action.id, name, *user_library, pose_asset_reference, *op->reports);

  library->catalog_service().write_to_disk(*final_full_asset_filepath);
  ensure_asset_ui_visible(*C);
  blender::ed::asset::shelf::show_catalog_in_visible_shelves(*C, catalog_path);

  BKE_id_free(bmain, &pose_action.id);

  blender::ed::asset::refresh_asset_library(C, lib_ref);

  WM_main_add_notifier(NC_ASSET | ND_ASSET_LIST | NA_ADDED, nullptr);

  return OPERATOR_FINISHED;
}

static int pose_asset_create_exec(bContext *C, wmOperator *op)
{
  char name[MAX_NAME] = "";
  PropertyRNA *name_prop = RNA_struct_find_property(op->ptr, "pose_name");
  if (RNA_property_is_set(op->ptr, name_prop)) {
    RNA_property_string_get(op->ptr, name_prop, name);
  }
  if (name[0] == '\0') {
    BKE_report(op->reports, RPT_ERROR, "No name set");
    return OPERATOR_CANCELLED;
  }

  const int enum_value = RNA_enum_get(op->ptr, "asset_library_reference");
  const AssetLibraryReference lib_ref = asset::library_reference_from_enum_value(enum_value);

  switch (lib_ref.type) {
    case ASSET_LIBRARY_LOCAL:
      return create_pose_asset_local(C, op, name, lib_ref);

    case ASSET_LIBRARY_CUSTOM:
      return create_pose_asset_user_library(C, op, name, lib_ref);

    default:
      /* Only local and custom libraries should be exposed in the enum. */
      BLI_assert_unreachable();
      break;
  }

  BKE_report(op->reports, RPT_ERROR, "Unexpected library type. Failed to create pose asset");

  return OPERATOR_FINISHED;
}

static int pose_asset_create_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  /* If the library isn't saved from the operator's last execution, use the first library. */
  if (!RNA_struct_property_is_set_ex(op->ptr, "asset_library_reference", false)) {
    const AssetLibraryReference first_library = blender::ed::asset::user_library_to_library_ref(
        *static_cast<const bUserAssetLibrary *>(U.asset_libraries.first));
    RNA_enum_set(op->ptr,
                 "asset_library_reference",
                 asset::library_reference_to_enum_value(&first_library));
  }

  return WM_operator_props_dialog_popup(C, op, 400, std::nullopt, IFACE_("Create"));
}

static bool pose_asset_create_poll(bContext *C)
{
  if (!ED_operator_posemode_context(C)) {
    return false;
  }
  if (BLI_listbase_is_empty(&U.asset_libraries)) {
    CTX_wm_operator_poll_msg_set(C, "No asset library available to save to");
    return false;
  }
  return true;
}

static void visit_library_prop_catalogs_catalog_for_search_fn(
    const bContext *C,
    PointerRNA *ptr,
    PropertyRNA * /*prop*/,
    const char *edit_text,
    FunctionRef<void(StringPropertySearchVisitParams)> visit_fn)
{
  const int enum_value = RNA_enum_get(ptr, "asset_library_reference");
  const AssetLibraryReference lib_ref = asset::library_reference_from_enum_value(enum_value);

  blender::ed::asset::visit_library_catalogs_catalog_for_search(
      *CTX_data_main(C), lib_ref, edit_text, visit_fn);
}

void POSELIB_OT_create_pose_asset(wmOperatorType *ot)
{
  ot->name = "Create Pose Asset";
  ot->description = "Create a new asset from the selection in the scene";
  ot->idname = "POSELIB_OT_create_pose_asset";

  ot->exec = pose_asset_create_exec;
  ot->invoke = pose_asset_create_invoke;
  ot->poll = pose_asset_create_poll;

  ot->prop = RNA_def_string(
      ot->srna, "pose_name", nullptr, MAX_NAME, "Pose Name", "Name for the new pose asset");

  PropertyRNA *prop = RNA_def_property(ot->srna, "asset_library_reference", PROP_ENUM, PROP_NONE);
  RNA_def_enum_funcs(prop, rna_asset_library_reference_itemf);
  RNA_def_property_ui_text(prop, "Library", "Asset library used to store the new pose");

  prop = RNA_def_string(
      ot->srna, "catalog_path", nullptr, MAX_NAME, "Catalog", "Catalog to use for the new asset");
  RNA_def_property_string_search_func_runtime(
      prop, visit_library_prop_catalogs_catalog_for_search_fn, PROP_STRING_SEARCH_SUGGESTION);

  /* This property is just kept to have backwards compatibility and has no functionality. It should
   * be removed in the 5.0 release. */
  prop = RNA_def_boolean(ot->srna,
                         "activate_new_action",
                         false,
                         "Activate New Action",
                         "This property is deprecated and will be removed in the future");
  RNA_def_property_flag(prop, PropertyFlag(PROP_HIDDEN | PROP_SKIP_SAVE));
}

enum AssetOverwriteMode {
  OVERWRITE_ADJUST = 0,
  OVERWRITE_REPLACE,
  OVERWRITE_ADD,
  OVERWRITE_REMOVE,
};

static const EnumPropertyItem prop_asset_overwrite_modes[] = {
    {OVERWRITE_ADJUST,
     "ADJUST",
     0,
     "Adjust",
     "Update existing channels in the pose asset but don't remove or add any channels"},
    {OVERWRITE_REPLACE,
     "REPLACE",
     0,
     "Replace",
     "Completely replace all channels in the pose asset with the current selection"},
    {OVERWRITE_ADD,
     "ADD",
     0,
     "Add",
     "Add channels of the selection to the pose asset. Existing channels will be updated"},
    {OVERWRITE_REMOVE,
     "REMOVE",
     0,
     "Remove",
     "Remove channels of the selection from the pose asset"},
    {0, nullptr, 0, nullptr, nullptr},
};

/* Gets the selected asset from the given `bContext`. If the asset is an action, returns a pointer
 * to that action, else returns a nullptr. */
static bAction *action_from_selected_asset(bContext *C)
{
  const AssetRepresentationHandle *asset_handle = CTX_wm_asset(C);
  if (!asset_handle) {
    return nullptr;
  }

  if (asset_handle->get_id_type() != ID_AC) {
    return nullptr;
  }

  AssetWeakReference asset_reference = asset_handle->make_weak_reference();
  Main *bmain = CTX_data_main(C);
  return reinterpret_cast<bAction *>(
      bke::asset_edit_id_from_weak_reference(*bmain, ID_AC, asset_reference));
}

struct PathValue {
  RNAPath rna_path;
  float value;
};

static Vector<PathValue> generate_path_values(Object &pose_object)
{
  Vector<PathValue> path_values;
  LISTBASE_FOREACH (bPoseChannel *, pose_bone, &pose_object.pose->chanbase) {
    if (!(pose_bone->bone->flag & BONE_SELECTED)) {
      continue;
    }
    PointerRNA bone_pointer = RNA_pointer_create(&pose_object.id, &RNA_PoseBone, pose_bone);
    Vector<RNAPath> rna_paths = blender::animrig::construct_keyframing_rna_paths(&bone_pointer);

    for (RNAPath &rna_path : rna_paths) {
      PointerRNA resolved_pointer;
      PropertyRNA *resolved_property;
      if (!RNA_path_resolve(
              &bone_pointer, rna_path.path.c_str(), &resolved_pointer, &resolved_property))
      {
        continue;
      }
      const std::optional<std::string> rna_path_id_to_prop = RNA_path_from_ID_to_property(
          &resolved_pointer, resolved_property);
      if (!rna_path_id_to_prop.has_value()) {
        continue;
      }
      Vector<float> values = blender::animrig::get_rna_values(&resolved_pointer,
                                                              resolved_property);
      int i = 0;
      for (const float value : values) {
        RNAPath path = {rna_path_id_to_prop.value(), std::nullopt, i};
        path_values.append({path, value});
        i++;
      }
    }
  }
  return path_values;
}

static void update_pose_action_from_scene(Main *bmain,
                                          blender::animrig::Action &action,
                                          Object &pose_object,
                                          const AssetOverwriteMode mode)
{
  using namespace blender::animrig;
  if (action.slot_array_num < 1) {
    /* All actions should have slots at this point. */
    BLI_assert_unreachable();
    return;
  }

  Set<RNAPath> existing_paths;
  foreach_fcurve_in_action_slot(action, action.slot_array[0]->handle, [&](FCurve &fcurve) {
    existing_paths.add({fcurve.rna_path, std::nullopt, fcurve.array_index});
  });

  KeyframeSettings key_settings = {BEZT_KEYTYPE_KEYFRAME, HD_AUTO, BEZT_IPO_BEZ};
  BLI_assert(action.strip_keyframe_data().size() == 1);
  BLI_assert(action.slots().size() == 1);
  StripKeyframeData *strip_data = action.strip_keyframe_data()[0];
  Slot *slot = action.slot(0);
  Vector<PathValue> path_values = generate_path_values(pose_object);

  switch (mode) {
    case OVERWRITE_ADJUST: {
      for (const PathValue &path_value : path_values) {
        /* Only updating existing channels. */
        if (existing_paths.contains(path_value.rna_path)) {
          strip_data->keyframe_insert(
              bmain,
              *slot,
              {path_value.rna_path.path, path_value.rna_path.index.value()},
              {1, path_value.value},
              key_settings);
        }
      }
      break;
    }
    case OVERWRITE_ADD: {
      for (const PathValue &path_value : path_values) {
        strip_data->keyframe_insert(bmain,
                                    *slot,
                                    {path_value.rna_path.path, path_value.rna_path.index.value()},
                                    {1, path_value.value},
                                    key_settings);
      }
      break;
    }
    case OVERWRITE_REPLACE: {
      Channelbag *channel_bag = strip_data->channelbag_for_slot(slot->handle);
      if (!channel_bag) {
        /* No channels to remove. */
        return;
      }
      channel_bag->fcurves_clear();
      for (const PathValue &path_value : path_values) {
        strip_data->keyframe_insert(bmain,
                                    *slot,
                                    {path_value.rna_path.path, path_value.rna_path.index.value()},
                                    {1, path_value.value},
                                    key_settings);
      }
      break;
    }
    case OVERWRITE_REMOVE: {
      Channelbag *channel_bag = strip_data->channelbag_for_slot(slot->handle);
      if (!channel_bag) {
        /* No channels to remove. */
        return;
      }
      Map<RNAPath, FCurve *> fcurve_map;
      foreach_fcurve_in_action_slot(action, action.slot_array[0]->handle, [&](FCurve &fcurve) {
        fcurve_map.add({fcurve.rna_path, std::nullopt, fcurve.array_index}, &fcurve);
      });
      for (const PathValue &path_value : path_values) {
        if (existing_paths.contains(path_value.rna_path)) {
          FCurve *fcurve = fcurve_map.lookup(path_value.rna_path);
          channel_bag->fcurve_remove(*fcurve);
        }
      }
      break;
    }
  }
}

static void refresh_asset_library(bContext *C)
{
  const AssetRepresentationHandle *asset_handle = CTX_wm_asset(C);
  AssetWeakReference asset_reference = asset_handle->make_weak_reference();
  bUserAssetLibrary *library = BKE_preferences_asset_library_find_by_name(
      &U, asset_reference.asset_library_identifier);
  blender::ed::asset::refresh_asset_library(C, *library);
}

static int pose_asset_modify_exec(bContext *C, wmOperator *op)
{
  bAction *action = action_from_selected_asset(C);
  BLI_assert_msg(action, "Poll should have checked action exists");

  Main *bmain = CTX_data_main(C);
  Object *pose_object = CTX_data_active_object(C);
  if (!pose_object || !pose_object->pose) {
    return OPERATOR_CANCELLED;
  }

  AssetOverwriteMode mode = AssetOverwriteMode(RNA_enum_get(op->ptr, "mode"));
  update_pose_action_from_scene(bmain, action->wrap(), *pose_object, mode);

  asset::generate_preview(C, &action->id);
  if (ID_IS_LINKED(action)) {
    /* Not needed for local assets. */
    bke::asset_edit_id_save(*bmain, action->id, *op->reports);
  }

  refresh_asset_library(C);

  WM_main_add_notifier(NC_ASSET | ND_ASSET_LIST | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static bool pose_asset_modify_poll(bContext *C)
{
  if (!ED_operator_posemode_context(C)) {
    return false;
  }

  bAction *action = action_from_selected_asset(C);

  if (!action) {
    return false;
  }

  if (!ID_IS_LINKED(action)) {
    return true;
  }

  if (!bke::asset_edit_id_is_editable(action->id)) {
    return false;
  }

  if (!bke::asset_edit_id_is_writable(action->id)) {
    CTX_wm_operator_poll_msg_set(C, "Asset blend file is not editable");
    return false;
  }

  return true;
}

static std::string pose_asset_modify_description(bContext * /* C */,
                                                 wmOperatorType * /* ot */,
                                                 PointerRNA *ptr)
{
  const int mode = RNA_enum_get(ptr, "mode");
  return std::string(prop_asset_overwrite_modes[mode].description);
}

/* Calling it overwrite instead of save because we aren't actually saving an opened asset. */
void POSELIB_OT_asset_modify(wmOperatorType *ot)
{
  ot->name = "Modify Pose Asset";
  ot->description =
      "Update the selected pose asset in the asset library from the currently selected bones. The "
      "mode defines how the asset is updated";
  ot->idname = "POSELIB_OT_asset_modify";

  ot->exec = pose_asset_modify_exec;
  ot->poll = pose_asset_modify_poll;
  ot->get_description = pose_asset_modify_description;

  RNA_def_enum(ot->srna,
               "mode",
               prop_asset_overwrite_modes,
               OVERWRITE_ADJUST,
               "Overwrite Mode",
               "Specify which parts of the pose asset are overwritten");
}

static bool pose_asset_delete_poll(bContext *C)
{
  if (!ED_operator_posemode_context(C)) {
    return false;
  }

  bAction *action = action_from_selected_asset(C);

  if (!action) {
    return false;
  }

  if (!ID_IS_LINKED(action)) {
    return true;
  }

  if (!bke::asset_edit_id_is_editable(action->id)) {
    return false;
  }

  return true;
}

static int pose_asset_delete_exec(bContext *C, wmOperator *op)
{
  bAction *action = action_from_selected_asset(C);
  if (!action) {
    return OPERATOR_CANCELLED;
  }

  const AssetRepresentationHandle *asset_handle = CTX_wm_asset(C);
  AssetWeakReference asset_reference = asset_handle->make_weak_reference();
  bUserAssetLibrary *library = BKE_preferences_asset_library_find_by_name(
      &U, asset_reference.asset_library_identifier);

  if (ID_IS_LINKED(action)) {
    bke::asset_edit_id_delete(*CTX_data_main(C), action->id, *op->reports);
  }
  else {
    asset::clear_id(&action->id);
  }

  blender::ed::asset::refresh_asset_library(C, *library);
  WM_main_add_notifier(NC_ASSET | ND_ASSET_LIST | NA_REMOVED, nullptr);

  return OPERATOR_FINISHED;
}

static int pose_asset_delete_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  bAction *action = action_from_selected_asset(C);

  return WM_operator_confirm_ex(
      C,
      op,
      IFACE_("Delete Pose Asset"),
      ID_IS_LINKED(action) ?
          IFACE_("Permanently delete pose asset blend file? This cannot be undone.") :
          IFACE_("Permanently delete pose asset? This cannot be undone."),
      IFACE_("Delete"),
      ALERT_ICON_WARNING,
      false);
}

void POSELIB_OT_asset_delete(wmOperatorType *ot)
{
  ot->name = "Delete Pose Asset";
  ot->description = "Delete the selected Pose Asset";
  ot->idname = "POSELIB_OT_asset_delete";

  ot->poll = pose_asset_delete_poll;
  ot->invoke = pose_asset_delete_invoke;
  ot->exec = pose_asset_delete_exec;
}

}  // namespace blender::ed::animrig