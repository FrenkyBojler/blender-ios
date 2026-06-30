/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include "DNA_userdef_types.h"

#include "ED_userpref.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"

#include "BKE_blender_project.hh"
#include "BLI_string_ref.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"

#ifdef RNA_RUNTIME

namespace blender {

static void project_mark_dirty(bke::BlenderProject *project)
{
  BLI_assert(project != nullptr);
  bke::with_blender_project_write_lock([&] { project->is_dirty = true; });
}

/* For properties that AREN'T saved to disk as part of the project data. */
static void rna_BlenderProject_ui_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA * /*ptr*/)
{
  /* Force full redraw of all windows. */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

/* For properties that ARE saved to disk as part of the project data. */
static void rna_BlenderProject_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
  project_mark_dirty(project);

  /* Force full redraw of all windows. */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

static void rna_BlenderProject_name_get(PointerRNA *ptr, char *value)
{
  bke::with_blender_project_read_lock([&] {
    const bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
    strcpy(value, project->get_name().c_str());
  });
}

static int rna_BlenderProject_name_length(PointerRNA *ptr)
{
  int name_length;
  bke::with_blender_project_read_lock([&] {
    const bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
    name_length = project->get_name().size();
  });
  return name_length;
}

static void rna_BlenderProject_name_set(PointerRNA *ptr, const char *value)
{
  bke::with_blender_project_write_lock([&] {
    bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);

    StringRef name = StringRef(value);

    if (name.is_empty()) {
      /* Leave the name as-is when passed an empty (which is invalid) name. */
      return;
    }

    project->set_name(name);
  });
}

static void rna_BlenderProject_root_path_get(PointerRNA *ptr, char *value)
{
  bke::with_blender_project_read_lock([&] {
    const bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
    strcpy(value, project->get_root_path().c_str());
  });
}

static int rna_BlenderProject_root_path_length(PointerRNA *ptr)
{
  int root_path_length;
  bke::with_blender_project_read_lock([&] {
    const bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
    root_path_length = project->get_root_path().size();
  });
  return root_path_length;
}

static bool rna_BlenderProject_is_dirty_get(PointerRNA *ptr)
{
  bool is_dirty;
  bke::with_blender_project_read_lock([&] {
    bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
    is_dirty = project->is_dirty;
  });
  return is_dirty;
}

static void rna_BlenderProject_is_dirty_set(PointerRNA *ptr, bool value)
{
  bke::with_blender_project_write_lock([&] {
    bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
    project->is_dirty = value;
  });
}

static bool skip_assetlist_item(CollectionPropertyIterator * /*iter*/, void *data)
{
  bUserAssetLibrary *asset_library = static_cast<bUserAssetLibrary *>(data);
  return !(asset_library->flag & ASSET_LIBRARY_PROJECT_DEFINED);
}

static void rna_BlenderProject_assetlist_begin(CollectionPropertyIterator *iter, PointerRNA *ptr)
{
  rna_iterator_listbase_begin(iter, ptr, &U.asset_libraries, skip_assetlist_item);
}

static int rna_BlenderProject_active_asset_get(PointerRNA *ptr)
{
  int active_index;
  bke::with_blender_project_read_lock([&] {
    const bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
    active_index = project->active_asset_library;
  });
  return active_index;
}

static void rna_BlenderProject_active_asset_set(PointerRNA *ptr, int value)
{
  bke::with_blender_project_write_lock([&] {
    bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
    project->active_asset_library = value;
  });
}

static bUserAssetLibrary *rna_BlenderProject_asset_library_new(const bContext *C,
                                                               const char *name,
                                                               const char *directory)
{
  bUserAssetLibrary *new_library;
  Main *bmain = CTX_data_main(C);

  BKE_blender_project_write_callback(bmain, [&](bke::BlenderProject *project) {
    new_library = ED_userpref_asset_library_new(
        C, name ? name : "", directory ? directory : "", bUserAssetLibraryAddType::Local, true);

    int project_asset_index = -1;
    for (bUserAssetLibrary &library : U.asset_libraries) {
      if (library.flag & ASSET_LIBRARY_PROJECT_DEFINED) {
        project_asset_index++;
      }
      if (&library == new_library) {
        break;
      }
    }

    project->active_asset_library = project_asset_index;
    project->is_dirty = true;
  });
  /* Force full redraw of all windows. (No notifier to redraw just the project asset windows yet)
   */
  WM_main_add_notifier(NC_WINDOW, nullptr);
  return new_library;
}

static void rna_BlenderProject_asset_library_remove(bContext *C,
                                                    ReportList *reports,
                                                    PointerRNA *ptr)
{
  bUserAssetLibrary *library = static_cast<bUserAssetLibrary *>(ptr->data);
  Main *bmain = CTX_data_main(C);

  BKE_blender_project_write_callback(bmain, [&](bke::BlenderProject *project) {
    if (BLI_findindex(&U.asset_libraries, library) == -1) {
      BKE_report(reports, RPT_ERROR, "Asset Library not found");
      return;
    }

    ED_userpref_asset_library_remove(C, library);

    int count_remaining = 0;
    for (bUserAssetLibrary &library : U.asset_libraries) {
      if (library.flag & ASSET_LIBRARY_PROJECT_DEFINED) {
        count_remaining++;
      }
    }
    CLAMP(project->active_asset_library, 0, count_remaining - 1);

    ptr->invalidate();
    project->is_dirty = true;
  });
  /* Force full redraw of all windows.(No notifier to redraw just the project asset windows yet) */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

}  // namespace blender

#else

namespace blender {

static void rna_def_project_asset_library(BlenderRNA *brna)
{
  StructRNA *srna;

  srna = RNA_def_struct(brna, "ProjectAssetLibrary", "UserAssetLibrary");
  RNA_def_struct_sdna(srna, "bUserAssetLibrary");
  RNA_def_struct_ui_text(srna,
                         "Project Asset Library",
                         "Settings to define a reusable library for Asset Browsers to use");
}

static void rna_def_project_asset_library_collection(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "ProjectAssetLibraryCollection");
  srna = RNA_def_struct(brna, "ProjectAssetLibraryCollection", nullptr);
  RNA_def_struct_ui_text(srna, "Project Asset Libraries", "Collection of project asset libraries");

  func = RNA_def_function(srna, "new", "rna_BlenderProject_asset_library_new");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_CONTEXT);
  RNA_def_function_ui_description(func, "Add a new Project Asset Library");
  RNA_def_string(func, "name", nullptr, sizeof(bUserAssetLibrary::name), "Name", "");
  RNA_def_string(func, "directory", nullptr, sizeof(bUserAssetLibrary::dirpath), "Directory", "");
  /* return type */
  parm = RNA_def_pointer(func, "library", "ProjectAssetLibrary", "", "Newly added asset library");
  RNA_def_function_return(func, parm);

  func = RNA_def_function(srna, "remove", "rna_BlenderProject_asset_library_remove");
  RNA_def_function_flag(func, FUNC_NO_SELF | FUNC_USE_CONTEXT | FUNC_USE_REPORTS);
  RNA_def_function_ui_description(func, "Remove a Project Asset Library");
  parm = RNA_def_pointer(func, "library", "ProjectAssetLibrary", "", "");
  RNA_def_parameter_flags(parm, PROP_NEVER_NULL, PARM_REQUIRED | PARM_RNAPTR);
  RNA_def_parameter_clear_flags(parm, PROP_THICK_WRAP, ParameterFlag(0));
}

static void rna_def_blender_project(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "BlenderProject", nullptr);
  RNA_def_struct_ui_text(srna, "Blender Project", "");

  PropertyRNA *prop;
  prop = RNA_def_property(srna, "is_dirty", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_BlenderProject_is_dirty_get", "rna_BlenderProject_is_dirty_set");
  RNA_def_property_ui_text(prop, "Dirty", "Whether the project has unsaved changes");
  RNA_def_property_update(prop, 0, "rna_BlenderProject_ui_update");

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_BlenderProject_name_get",
                                "rna_BlenderProject_name_length",
                                "rna_BlenderProject_name_set");
  RNA_def_property_ui_text(prop, "Name", "The project's name");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_update(prop, 0, "rna_BlenderProject_update");

  prop = RNA_def_property(srna, "root_path", PROP_STRING, PROP_DIRPATH);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_string_funcs(
      prop, "rna_BlenderProject_root_path_get", "rna_BlenderProject_root_path_length", nullptr);
  RNA_def_property_ui_text(prop, "Root Folder", "The path to the root folder of the project");

  prop = RNA_def_property(srna, "asset_libraries", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_collection_funcs(prop,
                                    "rna_BlenderProject_assetlist_begin",
                                    "rna_iterator_listbase_next",
                                    "rna_iterator_listbase_end",
                                    "rna_iterator_listbase_get",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_struct_type(prop, "ProjectAssetLibrary");
  RNA_def_property_ui_text(prop, "Project Asset Libraries", "");

  rna_def_project_asset_library_collection(brna, prop);
  rna_def_project_asset_library(brna);

  prop = RNA_def_property(srna, "active_asset_library", PROP_INT, PROP_NONE);
  RNA_def_property_int_funcs(
      prop, "rna_BlenderProject_active_asset_get", "rna_BlenderProject_active_asset_set", nullptr);
  RNA_def_property_ui_text(
      prop, "Active Asset Library", "Index of the asset library being edited in the Project UI");
}

void RNA_def_blender_project(BlenderRNA *brna)
{
  rna_def_blender_project(brna);
}

}  // namespace blender

#endif
