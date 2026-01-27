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
namespace bke {
struct DynamicOverrideRuntime;
}  // namespace bke

enum DynamicOverrideRuleType {
  UNKNOWN = 0,
  IDDATA = 1,
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

struct DynamicOverrideRule {
  struct DynamicOverrideRule *next = nullptr, *prev = nullptr;

  int16_t flag = 0;
  /** #DynamicOverrideRuleType. */
  int8_t type = 0;
  int8_t _pad[5] = {};
};

struct DynamicOverrideRuleIDData {
  DynamicOverrideRule base = {};

  ID *id_owner = nullptr;
  void *_pad = nullptr;

  ListBaseT<DynamicOverrideRuleProperty> properties = {nullptr, nullptr};
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

  bke::DynamicOverrideRuntime *runtime = nullptr;
  void *_pad = nullptr;
};

}  // namespace blender
