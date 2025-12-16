/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "rna_internal.hh"

#include "BKE_blender_project.hh"
#include "BLI_string_ref.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"

#ifdef RNA_RUNTIME

using namespace blender;

static void project_mark_dirty()
{
  BKE_blender_project().is_dirty = true;
}

/* For properties that AREN'T saved to disk as part of the project data. */
static void rna_BlenderProject_ui_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA * /*ptr*/)
{
  /* Force full redraw of all windows. */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

/* For properties that ARE saved to disk as part of the project data. */
static void rna_BlenderProject_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA * /*ptr*/)
{
  project_mark_dirty();

  /* Force full redraw of all windows. */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

static void rna_BlenderProjectData_name_get(PointerRNA *ptr, char *value)
{
  const bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);
  BLI_assert(project_data != nullptr);
  if (!project_data) {
    value[0] = '\0';
    return;
  }

  strcpy(value, project_data->get_name().c_str());
}

static int rna_BlenderProjectData_name_length(PointerRNA *ptr)
{
  const bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);
  BLI_assert(project_data != nullptr);
  if (!project_data) {
    return 0;
  }

  return project_data->get_name().size();
}

static void rna_BlenderProjectData_name_set(PointerRNA *ptr, const char *value)
{
  bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);
  BLI_assert(project_data != nullptr);
  if (!project_data) {
    return;
  }

  /* TODO: validate name. */

  project_data->set_name(value);
}

static void rna_BlenderProjectData_root_path_get(PointerRNA *ptr, char *value)
{
  const bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);
  BLI_assert(project_data != nullptr);
  if (!project_data) {
    value[0] = '\0';
    return;
  }

  strcpy(value, project_data->get_root_path().c_str());
}

static int rna_BlenderProjectData_root_path_length(PointerRNA *ptr)
{
  const bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);
  BLI_assert(project_data != nullptr);
  if (!project_data) {
    return 0;
  }

  return project_data->get_root_path().size();
}

static bool rna_BlenderProject_is_dirty_get(PointerRNA *ptr)
{
  bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
  return project->is_dirty;
}

static void rna_BlenderProject_is_dirty_set(PointerRNA *ptr, bool value)
{
  bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
  project->is_dirty = value;
}

static PointerRNA rna_BlenderProject_data_get(PointerRNA *ptr)
{
  bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
  BLI_assert(project != nullptr);
  if (!project || !project->data.has_value()) {
    return RNA_pointer_create_discrete(nullptr, &RNA_BlenderProjectData, nullptr);
  }

  return RNA_pointer_create_discrete(nullptr, &RNA_BlenderProjectData, &project->data);
}

static void rna_BlenderProject_init(PointerRNA ptr, const char *name, const char *project_root)
{
  bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr.data);
  BLI_assert(project != nullptr);
  if (!project) {
    return;
  }

  project->init(name, project_root);
}

static void rna_BlenderProject_clear(PointerRNA ptr)
{
  bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr.data);
  BLI_assert(project != nullptr);
  if (!project) {
    return;
  }

  project->clear();
}

#else

void rna_def_blender_project_data(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "BlenderProjectData", nullptr);
  RNA_def_struct_ui_text(srna, "Blender Project Data", "");

  PropertyRNA *prop;

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_BlenderProjectData_name_get",
                                "rna_BlenderProjectData_name_length",
                                "rna_BlenderProjectData_name_set");
  RNA_def_property_ui_text(prop, "Name", "The identifier for the project");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_update(prop, 0, "rna_BlenderProject_update");

  prop = RNA_def_property(srna, "root_path", PROP_STRING, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_string_funcs(prop,
                                "rna_BlenderProjectData_root_path_get",
                                "rna_BlenderProjectData_root_path_length",
                                nullptr);
  RNA_def_property_ui_text(prop, "Root Folder", "The path to the root folder of the project");
}

void rna_def_blender_project(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  FunctionRNA *func;
  PropertyRNA *parm;

  srna = RNA_def_struct(brna, "BlenderProject", nullptr);
  RNA_def_struct_ui_text(srna, "Blender Project", "");

  prop = RNA_def_property(srna, "data", PROP_POINTER, PROP_NONE);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_struct_type(prop, "BlenderProjectData");
  RNA_def_property_pointer_funcs(prop, "rna_BlenderProject_data_get", NULL, NULL, NULL);
  // RNA_def_property_update(prop, 0, "rna_BlenderProject_update");

  prop = RNA_def_property(srna, "is_dirty", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_BlenderProject_is_dirty_get", "rna_BlenderProject_is_dirty_set");
  RNA_def_property_ui_text(prop, "Dirty", "Whether the project has unsaved changes");
  RNA_def_property_update(prop, 0, "rna_BlenderProject_ui_update");

  func = RNA_def_function(srna, "init", "rna_BlenderProject_init");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA);
  parm = RNA_def_string(func, "name", nullptr, 0, nullptr, "TODO: description");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_string(func, "project_root", nullptr, 0, nullptr, "TODO: description");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "clear", "rna_BlenderProject_clear");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA);
}

void RNA_def_blender_project(BlenderRNA *brna)
{
  rna_def_blender_project(brna);
  rna_def_blender_project_data(brna);
}

#endif
