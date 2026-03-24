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
#include "BKE_path_templates.hh"
#include "BLI_string_ref.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"

namespace blender {

const EnumPropertyItem rna_enum_project_variable_type_items[] = {
    {int(bke::ProjectVarType::INTEGER), "INTEGER", 0, "Integer", "An integer variable"},
    {int(bke::ProjectVarType::FLOAT), "FLOAT", 0, "Float", "A floating point variable"},
    {int(bke::ProjectVarType::STRING), "STRING", 0, "String", "An string variable"},
    {int(bke::ProjectVarType::FILEPATH), "FILEPATH", 0, "Filepath", "A filepath variable"},
    {0, nullptr, 0, nullptr, nullptr},
};

}

#ifdef RNA_RUNTIME

namespace blender {

using namespace bke;

/* --------------------------------------------------------- */

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

/* --------------------------------------------------------- */

static int rna_ProjectVariable_type_get(PointerRNA *ptr)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);
  return int(var->type);
}

static void rna_ProjectVariable_type_set(PointerRNA *ptr, int value)
{
  ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);

  var->type = ProjectVarType(value);
}

static void rna_ProjectVariable_name_get(PointerRNA *ptr, char *value)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);

  strcpy(value, var->name.c_str());
}

static int rna_ProjectVariable_name_length(PointerRNA *ptr)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);

  return var->name.size();
}

static void rna_ProjectVariable_name_set(PointerRNA *ptr, const char *value)
{
  ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);

  std::string new_name(value);
  BKE_ensure_valid_variable_name(new_name);

  var->name = new_name;
}

static void rna_ProjectVariable_description_get(PointerRNA *ptr, char *value)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);

  strcpy(value, var->description.c_str());
}

static int rna_ProjectVariable_description_length(PointerRNA *ptr)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);

  return var->description.size();
}

static void rna_ProjectVariable_description_set(PointerRNA *ptr, const char *value)
{
  ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);

  var->description.clear();
  var->description.append(value);
}

static int rna_ProjectVariable_value_int_get(PointerRNA *ptr)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);
  return var->value_int;
}

static void rna_ProjectVariable_value_int_set(PointerRNA *ptr, int value)
{
  ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);
  var->value_int = value;
}

static float rna_ProjectVariable_value_float_get(PointerRNA *ptr)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);
  return var->value_float;
}

static void rna_ProjectVariable_value_float_set(PointerRNA *ptr, float value)
{
  ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);
  var->value_float = value;
}

static void rna_ProjectVariable_value_string_get(PointerRNA *ptr, char *value)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);

  strcpy(value, var->value_string.c_str());
}

static int rna_ProjectVariable_value_string_length(PointerRNA *ptr)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);

  return var->value_string.size();
}

static void rna_ProjectVariable_value_string_set(PointerRNA *ptr, const char *value)
{
  ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);

  var->value_string.clear();
  var->value_string.append(value);
}

/* --------------------------------------------------------- */

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

static int rna_BlenderProject_active_variable_get(PointerRNA *ptr)
{
  const bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);

  return project_data->active_variable;
}

static void rna_BlenderProject_active_variable_set(PointerRNA *ptr, int value)
{
  bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);

  project_data->active_variable = value;
}

static void rna_BlenderProject_active_variable_range(
    PointerRNA *ptr, int *min, int *max, int * /*softmin*/, int * /*softmax*/)
{
  const bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);

  *min = 0;
  *max = project_data->variables.size() - 1;
}

static void rna_iterator_BlenderProject_variables_begin(CollectionPropertyIterator *iter,
                                                        PointerRNA *ptr)
{
  bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);

  rna_iterator_array_begin(iter,
                           ptr,
                           (void *)project_data->variables.begin(),
                           sizeof(std::unique_ptr<ProjectVariable>),
                           project_data->variables.size(),
                           0,
                           nullptr);
}

static int rna_iterator_BlenderProject_variables_length(PointerRNA *ptr)
{
  const bke::BlenderProject *project_data = static_cast<bke::BlenderProject *>(ptr->data);
  return project_data->variables.size();
}

static PointerRNA rna_iterator_BlenderProject_variables_get(CollectionPropertyIterator *iter)
{
  BLI_assert(iter->valid);

  ArrayIterator *internal = &iter->internal.array;

  std::unique_ptr<ProjectVariable> *var_ptr_ptr =
      reinterpret_cast<std::unique_ptr<ProjectVariable> *>(internal->ptr);

  ProjectVariable *var_ptr = var_ptr_ptr->get();

  return RNA_pointer_create_with_parent(iter->parent, RNA_ProjectVariable, var_ptr);
}

static ProjectVariable *rna_ProjectVariables_new(bke::BlenderProject *project_data,
                                                 ReportList *reports,
                                                 const char *name,
                                                 int type)
{
  if (name[0] == 0) {
    BKE_reportf(reports, RPT_ERROR, "Invalid variable name '%s': name must not be empty.", name);
    return nullptr;
  }

  ProjectVariable *new_var = project_data->new_variable();
  new_var->name = std::string(name);
  new_var->description = std::string();
  new_var->type = bke::ProjectVarType(type);
  new_var->value_int = 0;
  new_var->value_float = 0.0;
  new_var->value_string = std::string();

  project_mark_dirty();

  return new_var;
}

void rna_ProjectVariables_remove(bke::BlenderProject *project_data,
                                 ReportList *reports,
                                 PointerRNA *variable_ptr)
{
  BLI_assert(variable_ptr->type == RNA_ProjectVariable);
  ProjectVariable *var = static_cast<ProjectVariable *>(variable_ptr->data);

  if (!project_data->remove_variable(var)) {
    BKE_reportf(reports, RPT_ERROR, "Variable not found in project variables.");
    return;
  }

  project_mark_dirty();
}

/* --------------------------------------------------------- */

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

void rna_def_project_variable(BlenderRNA *brna)
{
  StructRNA *srna = RNA_def_struct(brna, "ProjectVariable", nullptr);
  RNA_def_struct_ui_text(srna, "Blender Project Variable", "");

  PropertyRNA *prop;

  prop = RNA_def_property(srna, "name", PROP_STRING, PROP_NONE);
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_ui_text(prop, "Name", "The variable's name");
  RNA_def_property_string_funcs(prop,
                                "rna_ProjectVariable_name_get",
                                "rna_ProjectVariable_name_length",
                                "rna_ProjectVariable_name_set");
  RNA_def_property_update(prop, 0, "rna_BlenderProject_update");

  prop = RNA_def_property(srna, "description", PROP_STRING, PROP_NONE);
  RNA_def_property_ui_text(
      prop, "Description", "Description of the variable (e.g. purpose, semantics, etc.)");
  RNA_def_property_string_funcs(prop,
                                "rna_ProjectVariable_description_get",
                                "rna_ProjectVariable_description_length",
                                "rna_ProjectVariable_description_set");
  RNA_def_property_update(prop, 0, "rna_BlenderProject_update");

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_ui_text(prop, "Type", "The variable's data type");
  RNA_def_property_enum_items(prop, rna_enum_project_variable_type_items);
  RNA_def_property_enum_funcs(
      prop, "rna_ProjectVariable_type_get", "rna_ProjectVariable_type_set", nullptr);
  RNA_def_property_update(prop, 0, "rna_BlenderProject_update");

  prop = RNA_def_property(srna, "value_int", PROP_INT, PROP_NONE);
  RNA_def_property_ui_text(prop, "Value", "The variable's integer value");
  RNA_def_property_int_funcs(
      prop, "rna_ProjectVariable_value_int_get", "rna_ProjectVariable_value_int_set", nullptr);
  RNA_def_property_update(prop, 0, "rna_BlenderProject_update");

  prop = RNA_def_property(srna, "value_float", PROP_FLOAT, PROP_NONE);
  RNA_def_property_ui_text(prop, "Value", "The variable's floating point value");
  RNA_def_property_float_funcs(
      prop, "rna_ProjectVariable_value_float_get", "rna_ProjectVariable_value_float_set", nullptr);
  RNA_def_property_update(prop, 0, "rna_BlenderProject_update");

  prop = RNA_def_property(srna, "value_string", PROP_STRING, PROP_NONE);
  RNA_def_property_ui_text(prop, "Value", "The variable's string/path value");
  RNA_def_property_string_funcs(prop,
                                "rna_ProjectVariable_value_string_get",
                                "rna_ProjectVariable_value_string_length",
                                "rna_ProjectVariable_value_string_set");
  RNA_def_property_update(prop, 0, "rna_BlenderProject_update");
}

static void rna_def_ProjectVariables(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;

  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "ProjectVariables");
  srna = RNA_def_struct(brna, "ProjectVariables", nullptr);
  RNA_def_struct_sdna(srna, "BlenderProject");
  RNA_def_struct_ui_text(srna, "Project Variables", "Collection of project variables");

  /* BlenderProject.variables.new(...) */
  func = RNA_def_function(srna, "new", "rna_ProjectVariables_new");
  RNA_def_function_ui_description(func, "Add a new variable to the project");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_string(func, "name", "Variable", 0, "Name", "Name of the new variable");
  parm = RNA_def_enum(func,
                      "type",
                      rna_enum_project_variable_type_items,
                      int(bke::ProjectVarType::STRING),
                      "Variable Type",
                      "The data type of the variable");
  parm = RNA_def_pointer(
      func, "variable", "ProjectVariable", "", "Newly created project variable");
  RNA_def_function_return(func, parm);

  /* BlenderProject.variables.remove(variable) */
  func = RNA_def_function(srna, "remove", "rna_ProjectVariables_remove");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_function_ui_description(func, "Remove a variable from the project");
  parm = RNA_def_pointer(
      func, "variable", "ProjectVariable", "Variable", "The variable to remove");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED | PARM_RNAPTR);
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

  prop = RNA_def_property(srna, "active_variable", PROP_INT, PROP_NONE);
  RNA_def_property_int_funcs(prop,
                             "rna_BlenderProject_active_variable_get",
                             "rna_BlenderProject_active_variable_set",
                             "rna_BlenderProject_active_variable_range");
  RNA_def_property_ui_text(
      prop, "Active Project Variable", "Index of the currently active variable in the UI");

  /* Collection properties. */
  prop = RNA_def_property(srna, "variables", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "ProjectVariable");
  RNA_def_property_collection_funcs(prop,
                                    "rna_iterator_BlenderProject_variables_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_BlenderProject_variables_get",
                                    "rna_iterator_BlenderProject_variables_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop, "Project Variables", "The variables in this project");
  rna_def_ProjectVariables(brna, prop);
}

void RNA_def_blender_project(BlenderRNA *brna)
{
  rna_def_project_variable(brna);
  rna_def_blender_project(brna);
}

}  // namespace blender

#endif
