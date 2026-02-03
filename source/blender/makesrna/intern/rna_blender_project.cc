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

/* TODO: these are copied from rna_action.cc.  Should move them to a shared place. */
template<typename T>
static void rna_iterator_array_begin(CollectionPropertyIterator *iter,
                                     PointerRNA *ptr,
                                     Span<T *> items)
{
  rna_iterator_array_begin(iter, ptr, (void *)items.data(), sizeof(T *), items.size(), 0, nullptr);
}

template<typename T>
static void rna_iterator_array_begin(CollectionPropertyIterator *iter,
                                     PointerRNA *ptr,
                                     MutableSpan<T *> items)
{
  rna_iterator_array_begin(iter, ptr, (void *)items.data(), sizeof(T *), items.size(), 0, nullptr);
}

/* --------------------------------------------------------- */

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

/* --------------------------------------------------------- */

static int rna_ProjectVariable_type_get(PointerRNA *ptr)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(ptr->data);
  return int(var->type);
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

  var->name.clear();
  var->name.append(value);
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

static void rna_BlenderProjectData_name_get(PointerRNA *ptr, char *value)
{
  const bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);

  strcpy(value, project_data->get_name().c_str());
}

static int rna_BlenderProjectData_name_length(PointerRNA *ptr)
{
  const bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);

  return project_data->get_name().size();
}

static void rna_BlenderProjectData_name_set(PointerRNA *ptr, const char *value)
{
  bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);

  project_data->set_name(value);
}

static void rna_BlenderProjectData_root_path_get(PointerRNA *ptr, char *value)
{
  const bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);

  strcpy(value, project_data->get_root_path().c_str());
}

static int rna_BlenderProjectData_root_path_length(PointerRNA *ptr)
{
  const bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);

  return project_data->get_root_path().size();
}

static void rna_iterator_BlenderProjectData_variables_begin(CollectionPropertyIterator *iter,
                                                            PointerRNA *ptr)
{
  bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);

  // rna_iterator_array_begin(iter, ptr, project_data->variables.as_span());
  rna_iterator_array_begin(iter,
                           ptr,
                           (void *)project_data->variables.begin(),
                           sizeof(ProjectVariable),
                           project_data->variables.size(),
                           0,
                           nullptr);
}

static int rna_iterator_BlenderProjectData_variables_length(PointerRNA *ptr)
{
  const bke::BlenderProjectData *project_data = static_cast<bke::BlenderProjectData *>(ptr->data);
  return project_data->variables.size();
}

static ProjectVariable *rna_ProjectVariables_new(bke::BlenderProjectData *project_data,
                                                 ReportList *reports,
                                                 const char *name,
                                                 int type)
{
  if (name[0] == 0) {
    BKE_reportf(reports, RPT_ERROR, "Invalid variable name '%s': name must not be empty.", name);
    return nullptr;
  }

  project_data->variables.append(ProjectVariable{
      std::string(name),
      bke::ProjectVarType(type),
      0,
      0.0,
      std::string(),
  });

  project_mark_dirty();

  return &project_data->variables.last();
}

void rna_ProjectVariables_remove(bke::BlenderProjectData *project_data,
                                 ReportList *reports,
                                 PointerRNA *variable_ptr)
{
  const ProjectVariable *var = static_cast<ProjectVariable *>(variable_ptr->data);

  int index = -1;
  for (int i = 0; i < project_data->variables.size(); i++) {
    if (project_data->variables[i].name == var->name) {
      index = i;
      break;
    }
  }

  if (index == -1) {
    BKE_reportf(reports, RPT_ERROR, "Variable not found in project variables.");
    return;
  }

  project_data->variables.remove(index);

  project_mark_dirty();
}

/* --------------------------------------------------------- */

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
  if (!project->data.has_value()) {
    return RNA_pointer_create_discrete(nullptr, RNA_BlenderProjectData, nullptr);
  }

  return RNA_pointer_create_discrete(nullptr, RNA_BlenderProjectData, &project->data);
}

static void rna_BlenderProject_init(PointerRNA ptr,
                                    ReportList *reports,
                                    const char *name,
                                    const char *project_root)
{
  bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr.data);

  if (!project->init(name, project_root)) {
    BKE_reportf(reports,
                RPT_ERROR,
                "Failed to initialize project. Ensure that both the name and project_root "
                "parameters are non-empty.");
  }

  /* Force full redraw of all windows. */
  WM_main_add_notifier(NC_WINDOW, nullptr);
}

static void rna_BlenderProject_clear(PointerRNA ptr)
{
  bke::BlenderProject *project = static_cast<bke::BlenderProject *>(ptr.data);

  project->clear();

  /* Force full redraw of all windows. */
  WM_main_add_notifier(NC_WINDOW, nullptr);
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

  prop = RNA_def_property(srna, "type", PROP_ENUM, PROP_NONE);
  RNA_def_property_ui_text(prop, "Type", "The variable's data type");
  RNA_def_property_enum_items(prop, rna_enum_project_variable_type_items);
  RNA_def_property_enum_funcs(prop, "rna_ProjectVariable_type_get", nullptr, nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
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
  RNA_def_struct_sdna(srna, "BlenderProjectData");
  RNA_def_struct_ui_text(srna, "Project Variables", "Collection of project variables");

  /* BlenderProjectData.variables.new(...) */
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

  /* BlenderProjectData.variables.remove(variable) */
  func = RNA_def_function(srna, "remove", "rna_ProjectVariables_remove");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  RNA_def_function_ui_description(func, "Remove a variable from the project");
  parm = RNA_def_pointer(
      func, "variable", "ProjectVariable", "Variable", "The variable to remove");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED | PARM_RNAPTR);
}

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
  RNA_def_property_ui_text(prop, "Name", "The project's name");
  RNA_def_struct_name_property(srna, prop);
  RNA_def_property_update(prop, 0, "rna_BlenderProject_update");

  prop = RNA_def_property(srna, "root_path", PROP_STRING, PROP_DIRPATH);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_property_string_funcs(prop,
                                "rna_BlenderProjectData_root_path_get",
                                "rna_BlenderProjectData_root_path_length",
                                nullptr);
  RNA_def_property_ui_text(prop, "Root Folder", "The path to the root folder of the project");

  /* Collection properties. */
  prop = RNA_def_property(srna, "variables", PROP_COLLECTION, PROP_NONE);
  RNA_def_property_struct_type(prop, "ProjectVariable");
  RNA_def_property_collection_funcs(prop,
                                    "rna_iterator_BlenderProjectData_variables_begin",
                                    "rna_iterator_array_next",
                                    "rna_iterator_array_end",
                                    "rna_iterator_array_dereference_get",
                                    "rna_iterator_BlenderProjectData_variables_length",
                                    nullptr,
                                    nullptr,
                                    nullptr);
  RNA_def_property_ui_text(prop, "Project Variables", "The variables in this project");
  rna_def_ProjectVariables(brna, prop);
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

  prop = RNA_def_property(srna, "is_dirty", PROP_BOOLEAN, PROP_NONE);
  RNA_def_property_boolean_funcs(
      prop, "rna_BlenderProject_is_dirty_get", "rna_BlenderProject_is_dirty_set");
  RNA_def_property_ui_text(prop, "Dirty", "Whether the project has unsaved changes");
  RNA_def_property_update(prop, 0, "rna_BlenderProject_ui_update");

  func = RNA_def_function(srna, "init", "rna_BlenderProject_init");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_string(func, "name", nullptr, 0, nullptr, "The project's name");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  parm = RNA_def_string(
      func, "project_root", nullptr, 0, nullptr, "The filepath of the project's root folder");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "clear", "rna_BlenderProject_clear");
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA);
}

void RNA_def_blender_project(BlenderRNA *brna)
{
  rna_def_project_variable(brna);
  rna_def_blender_project(brna);
  rna_def_blender_project_data(brna);
}

}  // namespace blender

#endif
