/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cstdlib>

#include "BKE_icons.hh"

#include "RNA_define.hh"

#include "rna_internal.hh"

#ifdef RNA_RUNTIME

#  include "DNA_ID.h"
#  include "DNA_dynamic_override_types.h"

#  include "BKE_idprop.hh"
#  include "BKE_lib_dynamic_override.hh"

#  include "DEG_depsgraph.hh"

#  include "WM_api.hh"

#  include "RNA_path.hh"

namespace blender {

static StructRNA *rna_DynamicOverrideRuleProperty_refine(PointerRNA *ptr)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();

  if (!rule_prop->orig_value) {
    return RNA_DynamicOverrideRuleProperty;
  }

  switch (rule_prop->orig_value->type) {
    case IDP_ARRAY:
      switch (rule_prop->orig_value->subtype) {
        case IDP_FLOAT:
          return RNA_DynamicOverrideRulePropertyFloatArray;
      }
  }
  return RNA_DynamicOverrideRuleProperty;
}

void rna_DynamicOverride_rule_property_rna_path_get(PointerRNA *ptr, char *value)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  strcpy(value, (rule_prop->rna_path == nullptr) ? "" : rule_prop->rna_path);
}

int rna_DynamicOverride_rule_property_rna_path_length(PointerRNA *ptr)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  return (rule_prop->rna_path == nullptr) ? 0 : strlen(rule_prop->rna_path);
}

static int rna_DynamicOverride_rule_property_float_array_get_length(
    const PointerRNA *ptr, int length[RNA_MAX_ARRAY_DIMENSION], IDProperty *idprop)
{
  BLI_assert(IDP_array_float_get(idprop));

  length[0] = idprop->len;
  return length[0];
}

static void rna_DynamicOverride_rule_property_float_array_get(PointerRNA *ptr,
                                                              float *values,
                                                              IDProperty *idprop)
{
  float *data = IDP_array_float_get(idprop);
  const size_t len = idprop->len;

  memcpy(values, data, len * sizeof(*data));
}

static void rna_DynamicOverride_rule_property_float_array_set(PointerRNA *ptr,
                                                              const float *values,
                                                              IDProperty *idprop)
{
  float *data = IDP_array_float_get(idprop);
  const size_t len = idprop->len;

  memcpy(data, values, len * sizeof(*data));
}

static int rna_DynamicOverride_rule_property_override_value_float_array_get_length(
    const PointerRNA *ptr, int length[RNA_MAX_ARRAY_DIMENSION])
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  return rna_DynamicOverride_rule_property_float_array_get_length(
      ptr, length, rule_prop->new_value);
}

static void rna_DynamicOverride_rule_property_override_value_float_array_get(PointerRNA *ptr,
                                                                             float *values)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  rna_DynamicOverride_rule_property_float_array_get(ptr, values, rule_prop->new_value);
}

static void rna_DynamicOverride_rule_property_override_value_float_array_set(PointerRNA *ptr,
                                                                             const float *values)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  rna_DynamicOverride_rule_property_float_array_set(ptr, values, rule_prop->new_value);
}

static int rna_DynamicOverride_rule_property_original_value_float_array_get_length(
    const PointerRNA *ptr, int length[RNA_MAX_ARRAY_DIMENSION])
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  return rna_DynamicOverride_rule_property_float_array_get_length(
      ptr, length, rule_prop->orig_value);
}

static void rna_DynamicOverride_rule_property_original_value_float_array_get(PointerRNA *ptr,
                                                                             float *values)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  rna_DynamicOverride_rule_property_float_array_get(ptr, values, rule_prop->orig_value);
}

static void rna_DynamicOverride_rule_property_original_value_float_array_set(PointerRNA *ptr,
                                                                             const float *values)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  rna_DynamicOverride_rule_property_float_array_set(ptr, values, rule_prop->orig_value);
}

static DynamicOverrideRuleProperty *rna_DynamicOverride_rule_property_add(
    DynamicOverrideRule *dynamic_override_rule, ReportList *reports, const char *rna_path_str)
{
  RNAPath rna_path = {rna_path_str};
  DynamicOverrideRuleProperty *result = bke::dynamic_override_rule_rna_property_add(
      *dynamic_override_rule, rna_path);

  // WM_main_add_notifier(NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  return result;
}

static void rna_DynamicOverride_rule_property_remove(DynamicOverrideRule *dynamic_override_rule,
                                                     ReportList *reports,
                                                     DynamicOverrideRuleProperty *property)
{
  if (dynamic_override_rule->type != DynamicOverrideRuleType::IDDATA) {
    BKE_report(
        reports, RPT_ERROR, "This dynamic override rule cannot conatin RNA-based properties");
    return;
  }
  DynamicOverrideRuleIDData *dynamic_override_rule_iddata =
      reinterpret_cast<DynamicOverrideRuleIDData *>(dynamic_override_rule);
  if (BLI_findindex(&dynamic_override_rule_iddata->properties, property) == -1) {
    BKE_report(reports, RPT_ERROR, "Property does not exist in this dynamic override rule");
    return;
  }

  bke::dynamic_override_rule_property_remove(*dynamic_override_rule, property);

  // WM_main_add_notifier(NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
}

static StructRNA *rna_DynamicOverrideRule_refine(PointerRNA *ptr)
{
  DynamicOverrideRule *rule = ptr->data_as<DynamicOverrideRule>();

  switch (rule->type) {
    case DynamicOverrideRuleType::IDDATA:
      return RNA_DynamicOverrideRuleIDData;
  }
  return RNA_DynamicOverrideRule;
}

static DynamicOverrideRule *rna_DynamicOverride_rule_iddata_ensure(
    DynamicOverride *dynamic_override, ReportList *reports, ID *owner_id)
{
  DynamicOverrideRuleIDData &result = bke::dynamic_override_rule_ensure_for_id(*dynamic_override,
                                                                               *owner_id);

  // WM_main_add_notifier(NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  return &result.base;
}

static void rna_DynamicOverride_rule_remove(DynamicOverride *dynamic_override,
                                            ReportList *reports,
                                            DynamicOverrideRule *rule)
{
  if (BLI_findindex(&dynamic_override->rules, rule) == -1) {
    BKE_report(reports,
               RPT_ERROR,
               "Dynamic override rule does not exist in this dynamic override data-block");
    return;
  }

  bke::dynamic_override_rule_remove(*dynamic_override, rule);

  // WM_main_add_notifier(NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
}

}  // namespace blender

#else

namespace blender {

static void rna_def_dynamic_override_rule_property(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "DynamicOverrideRuleProperty", nullptr);
  RNA_def_struct_ui_text(
      srna,
      "Dynamic Override Rule Property",
      "Data for the override of an RNA property of the data matching this rule");
  RNA_def_struct_sdna(srna, "DynamicOverrideRuleProperty");
  RNA_def_struct_refine_func(srna, "rna_DynamicOverrideRuleProperty_refine");

  prop = RNA_def_string(srna,
                        "rna_path",
                        nullptr,
                        0,
                        "RNA Path",
                        "RNA path to the property of the ID affected by this rule");
  RNA_def_property_string_funcs(prop,
                                "rna_DynamicOverride_rule_property_rna_path_get",
                                "rna_DynamicOverride_rule_property_rna_path_length",
                                nullptr);
}

static void rna_def_dynamic_override_rule_property_float_array(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(
      brna, "DynamicOverrideRulePropertyFloatArray", "DynamicOverrideRuleProperty");
  RNA_def_struct_ui_text(
      srna,
      "Float Array Property",
      "Data for the override of a float array RNA property of the data matching this rule");
  RNA_def_struct_sdna(srna, "DynamicOverrideRuleProperty");

  RNA_define_verify_sdna(false);

  prop = RNA_def_property(srna, "override_value", PROP_FLOAT, PROP_NONE);
  RNA_def_property_flag(prop, PROP_DYNAMIC);
  RNA_def_property_array(prop, 0);
  RNA_def_property_dynamic_array_funcs(
      prop, "rna_DynamicOverride_rule_property_override_value_float_array_get_length");
  RNA_def_property_float_funcs(prop,
                               "rna_DynamicOverride_rule_property_override_value_float_array_get",
                               "rna_DynamicOverride_rule_property_override_value_float_array_set",
                               nullptr);
  RNA_def_property_ui_text(
      prop, "Override Value", "Array of floats defining the overridden value");

  prop = RNA_def_property(srna, "original_value", PROP_FLOAT, PROP_NONE);
  RNA_def_property_flag(prop, PROP_DYNAMIC);
  RNA_def_property_array(prop, 0);
  RNA_def_property_dynamic_array_funcs(
      prop, "rna_DynamicOverride_rule_property_original_value_float_array_get_length");
  RNA_def_property_float_funcs(prop,
                               "rna_DynamicOverride_rule_property_original_value_float_array_get",
                               "rna_DynamicOverride_rule_property_original_value_float_array_set",
                               nullptr);
  RNA_def_property_ui_text(prop, "Original Value", "Array of floats storing the original value");

  RNA_define_verify_sdna(true);
}

static void rna_def_dynamic_override_rule_properties(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "DynamicOverrideRuleProperties");
  srna = RNA_def_struct(brna, "DynamicOverrideRuleProperties", nullptr);
  RNA_def_struct_sdna(srna, "DynamicOverrideRule");
  RNA_def_struct_ui_text(
      srna, "Dynamic Override Rule Properties", "Collection of dynamic override rule properties");

  /* Add Property */
  func = RNA_def_function(srna, "add_for_rnapath", "rna_DynamicOverride_rule_property_add");
  RNA_def_function_ui_description(func, "Add a RNA path based property to the given rule");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func,
                         "property",
                         "DynamicOverrideRuleProperty",
                         "New Property",
                         "Newly created override property in the rule");
  RNA_def_function_return(func, parm);
  parm = RNA_def_string(
      func,
      "rna_path",
      nullptr,
      0,
      "RNA Path",
      "The RNA path from the data matched by this rule to the overridden property");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "remove", "rna_DynamicOverride_rule_property_remove");
  RNA_def_function_ui_description(func, "Remove and delete a rule property");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func,
                         "property",
                         "DynamicOverrideRuleProperty",
                         "Property",
                         "Override property to be deleted from the rule");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
}

static void rna_def_dynamic_override_rule_iddata(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "DynamicOverrideRuleIDData", "DynamicOverrideRule");
  RNA_def_struct_ui_text(srna,
                         "Dynamic Override IDData Rule",
                         "Dynamic override rule for a given data-block properties");
  RNA_def_struct_sdna(srna, "DynamicOverrideRuleIDData");

  prop = RNA_def_pointer(srna, "owner_id", "ID", "Owner ID", "The ID affected by this rule");

  prop = RNA_def_collection(srna,
                            "properties",
                            "DynamicOverrideRuleProperty",
                            "Properties",
                            "List of RNA-based dynamic override rule properties");
  // RNA_def_property_update(prop, NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  rna_def_dynamic_override_rule_properties(brna, prop);
}

static void rna_def_dynamic_override_rule(BlenderRNA *brna)
{
  StructRNA *srna;
  // PropertyRNA *prop;

  srna = RNA_def_struct(brna, "DynamicOverrideRule", nullptr);
  RNA_def_struct_ui_text(
      srna, "Dynamic Override Rule", "Base type for all types of dynamic override rules");
  RNA_def_struct_sdna(srna, "DynamicOverrideRule");
  RNA_def_struct_refine_func(srna, "rna_DynamicOverrideRule_refine");
}

static void rna_def_dynamic_override_rules(BlenderRNA *brna, PropertyRNA *cprop)
{
  StructRNA *srna;
  FunctionRNA *func;
  PropertyRNA *parm;

  RNA_def_property_srna(cprop, "DynamicOverrideRules");
  srna = RNA_def_struct(brna, "DynamicOverrideRules", nullptr);
  RNA_def_struct_sdna(srna, "DynamicOverride");
  RNA_def_struct_ui_text(srna, "Dynamic Override Rules", "Collection of dynamic override rules");

  /* Add Property */
  func = RNA_def_function(srna, "ensure_for_iddata", "rna_DynamicOverride_rule_iddata_ensure");
  RNA_def_function_ui_description(func,
                                  "Add a rule for the given owner ID, if it doesn't exist yet");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func,
      "rule",
      "DynamicOverrideRule",
      "New Rule",
      "Newly created dynamic override rule for the given owner ID, or the matching existing one");
  RNA_def_function_return(func, parm);
  parm = RNA_def_pointer(func, "owner_id", "ID", "Owner ID", "Datablock affected by the rule");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "remove", "rna_DynamicOverride_rule_remove");
  RNA_def_function_ui_description(func, "Remove and delete a rule");
  RNA_def_function_flag(func, FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func, "rule", "DynamicOverrideRule", "Rule", "Dynamic override rule to be deleted");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
}

static void rna_def_dynamic_override(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "DynamicOverride", "ID");
  RNA_def_struct_ui_text(
      srna, "Dynamic Override", "Set of rules overriding values of data at evaluation time");
  RNA_def_struct_sdna(srna, "DynamicOverride");
  RNA_def_struct_ui_icon(srna, ICON_DATA_ID);

  prop = RNA_def_collection(
      srna, "rules", "DynamicOverrideRule", "Rules", "List of dynamic override rules");
  // RNA_def_property_update(prop, NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  rna_def_dynamic_override_rules(brna, prop);
}

void RNA_def_dynamic_override(BlenderRNA *brna)
{
  rna_def_dynamic_override_rule_property(brna);
  rna_def_dynamic_override_rule_property_float_array(brna);

  rna_def_dynamic_override_rule(brna);
  rna_def_dynamic_override_rule_iddata(brna);

  rna_def_dynamic_override(brna);
}

}  // namespace blender

#endif
