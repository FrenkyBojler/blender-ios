/* SPDX-FileCopyrightText: 2026 Blender Authors
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
#include "BKE_global.hh"
#include "BLI_string_ref.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"

#ifdef RNA_RUNTIME

namespace blender {

static void project_mark_dirty()
{
  if (!G_MAIN->project) {
    return;
  }

  G_MAIN->project->is_dirty = true;
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

static void rna_BlenderProject_name_get(PointerRNA *ptr, char *value)
{
  const bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);

  strcpy(value, project_data->get_name().c_str());
}

static int rna_BlenderProject_name_length(PointerRNA *ptr)
{
  const bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);

  return project_data->get_name().size();
}

static void rna_BlenderProject_name_set(PointerRNA *ptr, const char *value)
{
  bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);

  if (strlen(value) == 0) {
    // Leave the name as-is when passed an empty (which is invalid) name.
    return;
  }

  project_data->set_name(value);
}

static void rna_BlenderProject_root_path_get(PointerRNA *ptr, char *value)
{
  const bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);

  strcpy(value, project_data->get_root_path().c_str());
}

static int rna_BlenderProject_root_path_length(PointerRNA *ptr)
{
  const bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);

  return project_data->get_root_path().size();
}

static bool rna_BlenderProject_is_dirty_get(PointerRNA *ptr)
{
  bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);
  return project_data->is_dirty;
}

static void rna_BlenderProject_is_dirty_set(PointerRNA *ptr, bool value)
{
  bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);
  project_data->is_dirty = value;
}

}  // namespace blender

#else

namespace blender {

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
}

void RNA_def_blender_project(BlenderRNA *brna)
{
  rna_def_blender_project(brna);
}

}  // namespace blender

#endif
