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

static void rna_BlenderProject_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA * /*ptr*/)
{
  /* TODO evaluate which props should send which notifiers. */
  /* Force full redraw of all windows. */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

static void rna_BlenderProject_name_get(PointerRNA *ptr, char *value)
{
  const bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
  BLI_assert(project != nullptr);
  if (!project) {
    value[0] = '\0';
    return;
  }

  strcpy(value, project->get_name().c_str());
}

static int rna_BlenderProject_name_length(PointerRNA *ptr)
{
  const bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
  BLI_assert(project != nullptr);
  if (!project) {
    return 0;
  }

  return project->get_name().size();
}

static void rna_BlenderProject_name_set(PointerRNA *ptr, const char *value)
{
  bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
  BLI_assert(project != nullptr);
  if (!project) {
    return;
  }

  /* TODO: validate name. */

  project->set_name(value);
}

static void rna_BlenderProject_root_path_get(PointerRNA *ptr, char *value)
{
  const bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
  BLI_assert(project != nullptr);
  if (!project) {
    value[0] = '\0';
    return;
  }

  strcpy(value, project->get_root_path().c_str());
}

static int rna_BlenderProject_root_path_length(PointerRNA *ptr)
{
  const bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
  BLI_assert(project != nullptr);
  if (!project) {
    return 0;
  }

  return project->get_root_path().size();
}

static void rna_BlenderProject_root_path_set(PointerRNA *ptr, const char *value)
{
  bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr->data);
  BLI_assert(project != nullptr);
  if (!project) {
    return;
  }

  /* TODO: validate path. */

  project->set_root_path(value);
}

#else

void RNA_def_blender_project(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "BlenderProject", nullptr);
  RNA_def_struct_ui_text(srna, "Blender Project", "");

  PropertyRNA *prop;

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_BlenderProject_name_get",
                                "rna_BlenderProject_name_length",
                                "rna_BlenderProject_name_set");
  RNA_def_property_ui_text(prop, "Name", "The identifier for the project");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_update(prop, 0, "rna_BlenderProject_update");

  prop = RNA_def_property(srna, "root_path", PROP_STRING, PROP_NONE);
  RNA_def_property_string_funcs(prop,
                                "rna_BlenderProject_root_path_get",
                                "rna_BlenderProject_root_path_length",
                                "rna_BlenderProject_root_path_set");
  RNA_def_property_ui_text(prop, "Location", "The location of the project on disk");
}

#endif
