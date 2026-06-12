/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edundo
 */

#include "CLG_log.h"

#include "DNA_ID.h"
#include "DNA_dynamic_override_types.h"

#include "BKE_context.hh"
#include "BKE_dynamic_override.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_report.hh"

#include "WM_api.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_path.hh"

#include "ED_id_management.hh"

namespace blender {

/** We only need this locally. */
static CLG_LogRef LOG = {"lib.id_management"};

namespace ui {

static wmOperatorStatus dynoverride_remove_rule_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);

  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(
      WM_operator_properties_id_lookup_from_name_or_session_uid(bmain, op->ptr, ID_OV));
  if (!dynoverride) {
    return OPERATOR_CANCELLED;
  }

  std::string rule_name = RNA_string_get(op->ptr, "rule_name");
  DynamicOverrideRule *rule = bke::dynoverride::rule_lookup_by_name(*dynoverride, rule_name);
  if (!rule) {
    return OPERATOR_CANCELLED;
  }

  bke::dynoverride::rule_remove(*bmain, *dynoverride, rule);

  WM_main_add_notifier(NC_ID | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static void UI_OT_dynoverride_remove_rule(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Remove DynamicOverride Rule";
  ot->idname = "UI_OT_dynoverride_remove_rule";
  ot->description = "Remove the given rule from the given Dynamic Override data-block";

  /* callbacks */
  ot->exec = dynoverride_remove_rule_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_int(ot->srna,
                     "session_uid",
                     0,
                     INT32_MIN,
                     INT32_MAX,
                     "Session UUID",
                     "Session UUID of the dynamic override to remove the rule from",
                     INT32_MIN,
                     INT32_MAX);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

  prop = RNA_def_string(
      ot->srna, "rule_name", nullptr, 0, "Rule Name", "Name of the rule to be removed");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);
}

static wmOperatorStatus dynoverride_remove_rule_property_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);

  DynamicOverride *dynoverride = id_cast<DynamicOverride *>(
      WM_operator_properties_id_lookup_from_name_or_session_uid(bmain, op->ptr, ID_OV));
  if (!dynoverride) {
    return OPERATOR_CANCELLED;
  }

  std::string rule_name = RNA_string_get(op->ptr, "rule_name");
  DynamicOverrideRule *rule = bke::dynoverride::rule_lookup_by_name(*dynoverride, rule_name);
  if (!rule) {
    return OPERATOR_CANCELLED;
  }
  if (rule->type != DynamicOverrideRuleType::IDData) {
    return OPERATOR_CANCELLED;
  }

  std::string property_rna_path = RNA_string_get(op->ptr, "property_rna_path");
  RNAPath rna_path = {property_rna_path};
  DynamicOverrideRuleProperty *property = bke::dynoverride::rule_rna_property_lookup(*rule,
                                                                                     rna_path);
  if (!property) {
    return OPERATOR_CANCELLED;
  }

  bke::dynoverride::rule_property_remove(*bmain, *dynoverride, *rule, property);
  if (reinterpret_cast<DynamicOverrideRuleIDData *>(rule)->properties.is_empty()) {
    bke::dynoverride::rule_remove(*bmain, *dynoverride, rule);
  }

  WM_main_add_notifier(NC_ID | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static void UI_OT_dynoverride_remove_rule_property(wmOperatorType *ot)
{
  PropertyRNA *prop;

  /* identifiers */
  ot->name = "Remove DynamicOverride Rule Property";
  ot->idname = "UI_OT_dynoverride_remove_rule_property";
  ot->description =
      "Remove the given property from the given rule from the given Dynamic Override data-block";

  /* callbacks */
  ot->exec = dynoverride_remove_rule_property_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  prop = RNA_def_int(ot->srna,
                     "session_uid",
                     0,
                     INT32_MIN,
                     INT32_MAX,
                     "Session UUID",
                     "Session UUID of the dynamic override to remove the rule from",
                     INT32_MIN,
                     INT32_MAX);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

  prop = RNA_def_string(
      ot->srna, "rule_name", nullptr, 0, "Rule Name", "Name of the rule to be removed");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);

  prop = RNA_def_string(ot->srna,
                        "property_rna_path",
                        nullptr,
                        0,
                        "Property RNA Path",
                        "RNA path affected by the property to be removed");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE | PROP_HIDDEN);
}

void operatortypes_dynamic_override()
{
  WM_operatortype_append(UI_OT_dynoverride_remove_rule);
  WM_operatortype_append(UI_OT_dynoverride_remove_rule_property);
}

}  // namespace ui

}  // namespace blender
