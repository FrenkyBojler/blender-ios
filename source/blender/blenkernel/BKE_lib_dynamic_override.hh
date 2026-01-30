/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 */

namespace blender {

struct Depsgraph;
struct DynamicOverride;
struct DynamicOverrideRule;
struct DynamicOverrideRuleIDData;
struct DynamicOverrideRuleProperty;
struct ID;
struct Main;
struct ReportList;
struct RNAPath;
struct Scene;

namespace bke {

DynamicOverrideRuleIDData &dynamic_override_rule_ensure_for_id(DynamicOverride &dynamic_override,
                                                               ID &id_owner);

void dynamic_override_rule_remove(DynamicOverride &dynamic_override,
                                  DynamicOverrideRule *existing_rule);
void dynamic_override_rule_remove_for_id(DynamicOverride &dynamic_override, ID &owner_id);

DynamicOverrideRuleProperty *dynamic_override_rule_rna_property_add(DynamicOverrideRule &rule,
                                                                    RNAPath &rna_path);

void dynamic_override_rule_property_remove(DynamicOverrideRule &rule,
                                           DynamicOverrideRuleProperty *existing_property);

}  // namespace bke

}  // namespace blender
