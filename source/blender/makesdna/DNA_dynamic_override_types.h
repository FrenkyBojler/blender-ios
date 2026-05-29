/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_listBase.h"

namespace blender {

struct IDProperty;
namespace bke::dynoverride {
struct Runtime;
struct RuleIDDataRuntime;
}  // namespace bke::dynoverride

/** Types of target filtering to select which data a given dynoverride rule applies to. */
enum class DynamicOverrideRuleTargetFilterType : int8_t {
  Unknown = 0,
  /** The rule applies to a single ID. */
  IDSingle = 1,
};

struct DynamicOverrideRuleTargetFilter {
  DynamicOverrideRuleTargetFilterType type = {};
  int8_t _pad[7] = {};

  /* Type-specific data. */

  /** For DynamicOverrideRuleTargetFilterType::IDSingle, the affected ID. */
  ID *target_id = nullptr;
};

enum class DynamicOverrideRuleType : int8_t {
  Unknown = 0,
  IDData = 1,
};

/**
 * Override rule, gathering a set of changes to apply to a same set of target IDs.
 */
struct DynamicOverrideRule {
  struct DynamicOverrideRule *next = nullptr, *prev = nullptr;

  /** Type of rule, also defines the type of `rule_data`. */
  DynamicOverrideRuleType type = {};
  int8_t _pad[7] = {};

  /** Define which ID(s) is/are affected by this rule. */
  DynamicOverrideRuleTargetFilter target_filter = {};

  /**
   * Type-specific override data (e.g. source and target IDs for remapping, or affected RNA
   * properties and their values).
   */
  void *rule_data = nullptr;
};

struct DynamicOverrideRuleProperty {
  struct DynamicOverrideRuleProperty *next = nullptr, *prev = nullptr;

  /**
   * Reference to affected data (RNA path from owner ID, plus optional item info for collection
   * properties).
   */
  char *rna_path = nullptr;
  char *sub_item_name = nullptr;
  int sub_item_index = -1;
  int _pad = 0;

  /** New overridden value, and original one. */
  IDProperty *new_value = nullptr;
  IDProperty *orig_value = nullptr;
};

struct DynamicOverrideRuleIDData {
  DynamicOverrideRule base = {};

  /** List of overridden properties (based on RNA paths). */
  ListBaseT<DynamicOverrideRuleProperty> properties = {nullptr, nullptr};

  /**
   * New overridden values for all properties above.
   *
   * \note: Uses automatic data layout matching the runtime-generated RNA Srna data stored in
   * `runtime`.
   */
  IDProperty *new_values = nullptr;
  /** Original values for all properties above, follow samw layout as in `new_values`. */
  IDProperty *orig_values = nullptr;

  bke::dynoverride::RuleIDDataRuntime *runtime = nullptr;
};

struct DynamicOverride {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_OV;
#endif

  ID id;
  /** Animation data (must be immediately after id for utilities to use it). */
  struct AnimData *adt = nullptr;

  ListBaseT<DynamicOverrideRule> rules = {nullptr, nullptr};

  bke::dynoverride::Runtime *runtime = nullptr;
  void *_pad = nullptr;
};

}  // namespace blender
