/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cstdlib>

#include "DNA_dynamic_override_types.h"

#include "BKE_icons.hh"

#include "WM_api.hh"

#include "RNA_define.hh"

#include "DEG_depsgraph_build.hh"

#include "rna_internal.hh"

#ifdef RNA_RUNTIME

#  include "DNA_ID.h"

#  include "BKE_dynamic_override.hh"
#  include "BKE_idprop.hh"
#  include "BKE_main_invariants.hh"

#  include "DEG_depsgraph.hh"

#  include "WM_api.hh"

#  include "RNA_path.hh"

namespace blender {

static StructRNA *rna_DynamicOverrideRuleTargetFilter_refine(PointerRNA *ptr)
{
  DynamicOverrideRuleTargetFilter *filter = ptr->data_as<DynamicOverrideRuleTargetFilter>();

  switch (filter->type) {
    case DynamicOverrideRuleTargetFilterType::IDSingle:
      return RNA_DynamicOverrideRuleTargetFilterIDSingle;
    case DynamicOverrideRuleTargetFilterType::Unknown:
      break;
  }
  return RNA_DynamicOverrideRuleTargetFilter;
}

void rna_DynamicOverrideRuleProperty_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  auto ancestor = RNA_struct_search_closest_ancestor_by_type(ptr, RNA_DynamicOverrideRule);
  if (!ancestor) {
    return;
  }
  const DynamicOverrideRule *rule = static_cast<DynamicOverrideRule *>(ancestor->data);

  DEG_id_tag_update(rule->target_filter.target_id, ID_RECALC_DYNAMIC_OVERRIDE);
}

void rna_DynamicOverrideRuleProperty_is_muted_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  rna_DynamicOverrideRuleProperty_update(bmain, scene, ptr);
  DEG_relations_tag_update(bmain);
}

static void rna_DynamicOverride_rule_property_rna_path_get(PointerRNA *ptr, char *value)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  strcpy(value, (rule_prop->rna_path == nullptr) ? "" : rule_prop->rna_path);
}

static int rna_DynamicOverride_rule_property_rna_path_length(PointerRNA *ptr)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  return (rule_prop->rna_path == nullptr) ? 0 : strlen(rule_prop->rna_path);
}

static void rna_DynamicOverride_rule_property_propidentifier_get(PointerRNA *ptr, char *value)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  std::string safe_property_identifier = bke::dynoverride::rule_property_rna_identifier(
      *rule_prop);
  BLI_strncpy(value, safe_property_identifier.c_str(), safe_property_identifier.size() + 1);
}

static int rna_DynamicOverride_rule_property_propidentifier_length(PointerRNA *ptr)
{
  DynamicOverrideRuleProperty *rule_prop = ptr->data_as<DynamicOverrideRuleProperty>();
  std::string safe_property_identifier = bke::dynoverride::rule_property_rna_identifier(
      *rule_prop);
  return safe_property_identifier.size() + 1;
}

static void rna_DynamicOverride_rule_property_reset(ID *self_id,
                                                    PointerRNA self_ptr,
                                                    Main * /*bmain*/,
                                                    ReportList *reports)
{
  auto ancestor = RNA_struct_search_closest_ancestor_by_type(&self_ptr, RNA_DynamicOverrideRule);
  if (!ancestor) {
    BKE_report(reports, RPT_ERROR, "Cannot find the dynamic override rule owning this property");
    return;
  }
  DynamicOverrideRule *rule = static_cast<DynamicOverrideRule *>(ancestor->data);
  DynamicOverrideRuleProperty *rule_prop = self_ptr.data_as<DynamicOverrideRuleProperty>();

  bke::dynoverride::rule_rna_property_reset(
      *id_cast<DynamicOverride *>(self_id), *rule, *rule_prop);
}

static void rna_DynamicOverride_rule_property_apply_to_target(ID *self_id,
                                                              PointerRNA self_ptr,
                                                              Main * /*bmain*/,
                                                              ReportList *reports)
{
  auto ancestor = RNA_struct_search_closest_ancestor_by_type(&self_ptr, RNA_DynamicOverrideRule);
  if (!ancestor) {
    BKE_report(reports, RPT_ERROR, "Cannot find the dynamic override rule owning this property");
    return;
  }
  DynamicOverrideRule *rule = static_cast<DynamicOverrideRule *>(ancestor->data);
  DynamicOverrideRuleProperty *rule_prop = self_ptr.data_as<DynamicOverrideRuleProperty>();

  bke::dynoverride::rule_rna_property_apply_to_target(
      *id_cast<DynamicOverride *>(self_id), *rule, *rule_prop);
}

static void rna_DynamicOverride_rule_property_update_from_target(ID *self_id,
                                                                 PointerRNA self_ptr,
                                                                 Main * /*bmain*/,
                                                                 ReportList *reports)
{
  auto ancestor = RNA_struct_search_closest_ancestor_by_type(&self_ptr, RNA_DynamicOverrideRule);
  if (!ancestor) {
    BKE_report(reports, RPT_ERROR, "Cannot find the dynamic override rule owning this property");
    return;
  }
  DynamicOverrideRule *rule = static_cast<DynamicOverrideRule *>(ancestor->data);
  DynamicOverrideRuleProperty *rule_prop = self_ptr.data_as<DynamicOverrideRuleProperty>();

  bke::dynoverride::rule_rna_property_apply_to_target(
      *id_cast<DynamicOverride *>(self_id), *rule, *rule_prop);
}

static PointerRNA rna_DynamicOverride_rule_property_add(ID *self_id,
                                                        PointerRNA self_ptr,
                                                        Main *bmain,
                                                        ReportList * /*reports*/,
                                                        const char *rna_path_str)
{
  RNAPath rna_path = {rna_path_str};

  DynamicOverrideRuleProperty *result = bke::dynoverride::rule_rna_property_add(
      *bmain,
      *id_cast<DynamicOverride *>(self_id),
      *self_ptr.data_as<DynamicOverrideRule>(),
      rna_path);

  // WM_main_add_notifier(NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  return RNA_pointer_create_with_parent(self_ptr, RNA_DynamicOverrideRuleProperty, result);
}

static void rna_DynamicOverride_rule_property_remove(ID *self_id,
                                                     DynamicOverrideRule *dynamic_override_rule,
                                                     Main *bmain,
                                                     ReportList *reports,
                                                     DynamicOverrideRuleProperty *property)
{
  if (dynamic_override_rule->type != DynamicOverrideRuleType::IDData) {
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

  bke::dynoverride::rule_property_remove(
      *bmain, *id_cast<DynamicOverride *>(self_id), *dynamic_override_rule, property);

  // WM_main_add_notifier(NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
}

static StructRNA *rna_DynamicOverrideRule_override_values_refine(PointerRNA *ptr)
{
  auto iddata_rule = ptr->data_as<DynamicOverrideRuleIDData>();
  BLI_assert(iddata_rule->base.type == DynamicOverrideRuleType::IDData);
  return bke::dynoverride::rule_get_runtime_override_values_rna_struct(*iddata_rule);
}

static IDProperty **rna_DynamicOverrideRule_override_values_system_idprops(PointerRNA *ptr)
{
  auto iddata_rule = ptr->data_as<DynamicOverrideRuleIDData>();
  BLI_assert(iddata_rule->base.type == DynamicOverrideRuleType::IDData);
  return &iddata_rule->override_values;
}

static PointerRNA rna_DynamicOverrideRule_override_values_get(PointerRNA *ptr)
{
  auto iddata_rule = ptr->data_as<DynamicOverrideRuleIDData>();
  BLI_assert(iddata_rule->base.type == DynamicOverrideRuleType::IDData);
  return RNA_pointer_create_with_parent(
      *ptr, RNA_DynamicOverrideRuleIDDataOverrideValues, iddata_rule);
}

static StructRNA *rna_DynamicOverrideRule_original_values_refine(PointerRNA *ptr)
{
  auto iddata_rule = ptr->data_as<DynamicOverrideRuleIDData>();
  BLI_assert(iddata_rule->base.type == DynamicOverrideRuleType::IDData);
  return bke::dynoverride::rule_get_runtime_original_values_rna_struct(*iddata_rule);
}

static IDProperty **rna_DynamicOverrideRule_original_values_system_idprops(PointerRNA *ptr)
{
  auto iddata_rule = ptr->data_as<DynamicOverrideRuleIDData>();
  BLI_assert(iddata_rule->base.type == DynamicOverrideRuleType::IDData);
  return &iddata_rule->original_values;
}

static PointerRNA rna_DynamicOverrideRule_original_values_get(PointerRNA *ptr)
{
  auto iddata_rule = ptr->data_as<DynamicOverrideRuleIDData>();
  BLI_assert(iddata_rule->base.type == DynamicOverrideRuleType::IDData);
  return RNA_pointer_create_with_parent(
      *ptr, RNA_DynamicOverrideRuleIDDataOriginalValues, iddata_rule);
}

void rna_DynamicOverrideRule_update(Main * /*bmain*/, Scene * /*scene*/, PointerRNA *ptr)
{
  DynamicOverrideRule *rule = ptr->data_as<DynamicOverrideRule>();

  DEG_id_tag_update(rule->target_filter.target_id, ID_RECALC_DYNAMIC_OVERRIDE);
}

void rna_DynamicOverrideRule_is_muted_update(Main *bmain, Scene *scene, PointerRNA *ptr)
{
  DynamicOverrideRule *rule = ptr->data_as<DynamicOverrideRule>();
  if (rule->target_filter.target_id) {
    /* TODO: Not quite sure what's the right combination of tags yet to make muting the entire rule
     * work properly. */
    DEG_id_tag_update(rule->target_filter.target_id, ID_RECALC_SYNC_TO_EVAL);
    DEG_id_tag_update(rule->target_filter.target_id, ID_RECALC_PARAMETERS);
    DEG_id_tag_update(rule->target_filter.target_id, ID_RECALC_TRANSFORM);
  }
  rna_DynamicOverrideRule_update(bmain, scene, ptr);
  DEG_relations_tag_update(bmain);
}

static StructRNA *rna_DynamicOverrideRule_refine(PointerRNA *ptr)
{
  DynamicOverrideRule *rule = ptr->data_as<DynamicOverrideRule>();

  switch (rule->type) {
    case DynamicOverrideRuleType::IDData:
      return RNA_DynamicOverrideRuleIDData;
    case DynamicOverrideRuleType::Unknown:
      break;
  }
  return RNA_DynamicOverrideRule;
}

static void rna_DynamicOverride_rule_name_get(PointerRNA *ptr, char *value)
{
  DynamicOverrideRule *rule = ptr->data_as<DynamicOverrideRule>();
  strcpy(value, (rule->name == nullptr) ? "" : rule->name);
}

static int rna_DynamicOverride_rule_name_length(PointerRNA *ptr)
{
  DynamicOverrideRule *rule = ptr->data_as<DynamicOverrideRule>();
  return (rule->name == nullptr) ? 0 : strlen(rule->name);
}

static void rna_DynamicOverride_rule_name_set(PointerRNA *ptr, const char *value)
{
  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(ptr->owner_id);
  DynamicOverrideRule *rule = ptr->data_as<DynamicOverrideRule>();
  bke::dynoverride::rule_name_set(*dynoverride, *rule, value);
}

static PointerRNA rna_DynamicOverride_rule_iddata_ensure(PointerRNA self_ptr,
                                                         ReportList * /*reports*/,
                                                         ID *target_id)
{
  DynamicOverrideRuleIDData &result = bke::dynoverride::rule_iddata_ensure_for_id(
      *self_ptr.data_as<DynamicOverride>(), *target_id);

  // WM_main_add_notifier(NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  return RNA_pointer_create_with_parent(self_ptr, RNA_DynamicOverrideRuleIDData, &result.base);
}

static void rna_DynamicOverride_rule_remove(DynamicOverride *dynamic_override,
                                            Main *bmain,
                                            ReportList *reports,
                                            DynamicOverrideRule *rule)
{
  if (BLI_findindex(&dynamic_override->rules, rule) == -1) {
    BKE_report(reports,
               RPT_ERROR,
               "Dynamic override rule does not exist in this dynamic override data-block");
    return;
  }

  bke::dynoverride::rule_remove(*bmain, *dynamic_override, rule);

  // WM_main_add_notifier(NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
}

}  // namespace blender

#else

namespace blender {

static void rna_def_dynamic_override_rule_target_filter(BlenderRNA *brna)
{
  StructRNA *srna;

  srna = RNA_def_struct(brna, "DynamicOverrideRuleTargetFilter", nullptr);
  RNA_def_struct_ui_text(
      srna, "Dynamic Override Rule Target Filter", "Define which ID(s) are affected by this rule");
  RNA_def_struct_sdna(srna, "DynamicOverrideRuleTargetFilter");
  RNA_def_struct_refine_func(srna, "rna_DynamicOverrideRuleTargetFilter_refine");
}

static void rna_def_dynamic_override_rule_target_filter_single_id(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(
      brna, "DynamicOverrideRuleTargetFilterIDSingle", "DynamicOverrideRuleTargetFilter");
  RNA_def_struct_ui_text(srna,
                         "Dynamic Override Rule Target Filter Single ID",
                         "Rules with this type of filter only affect a single ID");
  RNA_def_struct_sdna(srna, "DynamicOverrideRuleTargetFilter");

  prop = RNA_def_pointer(srna, "target_id", "ID", "Target ID", "Data-block affected by this rule");
  RNA_def_property_clear_flag(prop, PROP_ID_REFCOUNT);
}

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
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);
  RNA_def_struct_name_property(srna, prop);

  prop = RNA_def_boolean(
      srna, "is_muted", false, "Muted", "Whether this override property is muted or not");
  RNA_def_property_boolean_sdna(
      prop, nullptr, "flag", int64_t(DynamicOverrideRulePropertyFlag::IsMuted));
  RNA_def_property_update(
      prop, NC_ID | NA_EDITED, "rna_DynamicOverrideRuleProperty_is_muted_update");

  RNA_define_verify_sdna(false);

  prop = RNA_def_string(
      srna,
      "property_identifier",
      nullptr,
      0,
      "Property Identifier",
      "Name of the RNA property dynamically generated to represent this data in the current "
      "Dynamic Override Rule, and store the matching new override values");
  RNA_def_property_string_funcs(prop,
                                "rna_DynamicOverride_rule_property_propidentifier_get",
                                "rna_DynamicOverride_rule_property_propidentifier_length",
                                nullptr);
  RNA_def_property_clear_flag(prop, PROP_EDITABLE);

  RNA_define_verify_sdna(true);

  FunctionRNA *func;

  func = RNA_def_function(srna, "reset", "rna_DynamicOverride_rule_property_reset");
  RNA_def_function_ui_description(
      func,
      "Reset the override value to the original value (at the time the override was created)");
  RNA_def_function_flag(func,
                        FUNC_USE_MAIN | FUNC_USE_SELF_ID | FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);

  func = RNA_def_function(
      srna, "apply_to_target", "rna_DynamicOverride_rule_property_apply_to_target");
  RNA_def_function_ui_description(func, "Apply the override value to the target overridden data");
  RNA_def_function_flag(func,
                        FUNC_USE_MAIN | FUNC_USE_SELF_ID | FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);

  func = RNA_def_function(
      srna, "update_from_target", "rna_DynamicOverride_rule_property_update_from_target");
  RNA_def_function_ui_description(func,
                                  "Update the override value from the target overridden data");
  RNA_def_function_flag(func,
                        FUNC_USE_MAIN | FUNC_USE_SELF_ID | FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
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
  RNA_def_function_flag(func,
                        FUNC_USE_MAIN | FUNC_USE_SELF_ID | FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func,
                         "property",
                         "DynamicOverrideRuleProperty",
                         "New Property",
                         "Newly created override property in the rule");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_RNAPTR);
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
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_SELF_ID | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(func,
                         "property",
                         "DynamicOverrideRuleProperty",
                         "Property",
                         "Override property to be deleted from the rule");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
}

static void rna_def_dynamic_override_rule_iddata_value_storage(BlenderRNA *brna)
{
  StructRNA *srna;

  srna = RNA_def_struct(brna, "DynamicOverrideRuleIDDataOverrideValues", nullptr);
  RNA_def_struct_ui_text(
      srna,
      "Dynamic Override IDData Rule Property Override Values",
      "Dynamically-defined struct representing the overridden properties and their value storage");
  RNA_def_struct_sdna(srna, "DynamicOverrideRuleIDData");
  RNA_def_struct_refine_func(srna, "rna_DynamicOverrideRule_override_values_refine");
  RNA_def_struct_system_idprops_func(srna,
                                     "rna_DynamicOverrideRule_override_values_system_idprops");

  srna = RNA_def_struct(brna, "DynamicOverrideRuleIDDataOriginalValues", nullptr);
  RNA_def_struct_ui_text(srna,
                         "Dynamic Override IDData Rule Property Original Values",
                         "Dynamically-defined struct representing the overridden properties and "
                         "their original value storage");
  RNA_def_struct_sdna(srna, "DynamicOverrideRuleIDData");
  RNA_def_struct_refine_func(srna, "rna_DynamicOverrideRule_original_values_refine");
  RNA_def_struct_system_idprops_func(srna,
                                     "rna_DynamicOverrideRule_original_values_system_idprops");
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

  RNA_define_verify_sdna(false);

  prop = RNA_def_pointer(
      srna,
      "override_values",
      "DynamicOverrideRuleIDDataOverrideValues",
      "Override Values",
      "Dynamically generated container for all override values, matching the override properties");
  RNA_def_property_pointer_funcs(
      prop, "rna_DynamicOverrideRule_override_values_get", nullptr, nullptr, nullptr);

  prop = RNA_def_pointer(
      srna,
      "original_values",
      "DynamicOverrideRuleIDDataOriginalValues",
      "Original Values",
      "Dynamically generated container for all original values (values of the target properties "
      "at the time the overrides were created), matching the override properties");
  RNA_def_property_pointer_funcs(
      prop, "rna_DynamicOverrideRule_original_values_get", nullptr, nullptr, nullptr);

  RNA_define_verify_sdna(true);

  prop = RNA_def_collection(srna,
                            "properties",
                            "DynamicOverrideRuleProperty",
                            "Properties",
                            "List of RNA-based dynamic override rule properties");
  // RNA_def_property_update(prop, NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  rna_def_dynamic_override_rule_properties(brna, prop);

  rna_def_dynamic_override_rule_iddata_value_storage(brna);
}

static void rna_def_dynamic_override_rule(BlenderRNA *brna)
{
  StructRNA *srna;
  PropertyRNA *prop;

  srna = RNA_def_struct(brna, "DynamicOverrideRule", nullptr);
  RNA_def_struct_ui_text(
      srna, "Dynamic Override Rule", "Base type for all types of dynamic override rules");
  RNA_def_struct_sdna(srna, "DynamicOverrideRule");
  RNA_def_struct_refine_func(srna, "rna_DynamicOverrideRule_refine");

  prop = RNA_def_string(srna,
                        "name",
                        nullptr,
                        0,
                        "Name",
                        "Name (and unique identifier within its DynamicOverride owner) of a rule");
  RNA_def_property_string_funcs(prop,
                                "rna_DynamicOverride_rule_name_get",
                                "rna_DynamicOverride_rule_name_length",
                                "rna_DynamicOverride_rule_name_set");
  RNA_def_struct_name_property(srna, prop);

  prop = RNA_def_boolean(
      srna, "is_muted", false, "Muted", "Whether this override rule is muted or not");
  RNA_def_property_boolean_sdna(prop, nullptr, "flag", int64_t(DynamicOverrideRuleFlag::IsMuted));
  RNA_def_property_update(prop, NC_ID | NA_EDITED, "rna_DynamicOverrideRule_is_muted_update");

  RNA_def_pointer(srna,
                  "target_filter",
                  "DynamicOverrideRuleTargetFilter",
                  "Target Filter",
                  "Set of options to control which IDs are affected by this rule");
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
  RNA_def_function_flag(func, FUNC_SELF_AS_RNA | FUNC_USE_REPORTS);
  parm = RNA_def_pointer(
      func,
      "rule",
      "DynamicOverrideRule",
      "New Rule",
      "Newly created dynamic override rule for the given target ID, or the matching existing one");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_RNAPTR);
  RNA_def_function_return(func, parm);
  parm = RNA_def_pointer(func, "target_id", "ID", "Target ID", "Data-block affected by the rule");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);

  func = RNA_def_function(srna, "remove", "rna_DynamicOverride_rule_remove");
  RNA_def_function_ui_description(func, "Remove and delete a rule");
  RNA_def_function_flag(func, FUNC_USE_MAIN | FUNC_USE_REPORTS);
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
  RNA_def_property_update(prop, NC_WM | ND_LIB_OVERRIDE_CHANGED, nullptr);
  rna_def_dynamic_override_rules(brna, prop);
}

void RNA_def_dynamic_override(BlenderRNA *brna)
{
  rna_def_dynamic_override_rule_target_filter(brna);
  rna_def_dynamic_override_rule_target_filter_single_id(brna);

  rna_def_dynamic_override_rule_property(brna);

  rna_def_dynamic_override_rule(brna);
  rna_def_dynamic_override_rule_iddata(brna);

  rna_def_dynamic_override(brna);
}

}  // namespace blender

#endif
