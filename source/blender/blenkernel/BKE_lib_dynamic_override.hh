/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 * SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_set.hh"
#include "BLI_vector_set.hh"

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
struct ViewLayer;

namespace bke {

/* -------------------------------------------------------------------- */
/** \name Basic Rule Management.
 * \{ */

DynamicOverrideRuleIDData &dynamic_override_rule_ensure_for_id(DynamicOverride &dynamic_override,
                                                               ID &id_owner);

void dynamic_override_rule_remove(DynamicOverride &dynamic_override,
                                  DynamicOverrideRule *existing_rule);
void dynamic_override_rule_remove_for_id(DynamicOverride &dynamic_override, ID &owner_id);

DynamicOverrideRuleProperty *dynamic_override_rule_rna_property_add(DynamicOverrideRule &rule,
                                                                    RNAPath &rna_path);

void dynamic_override_rule_property_remove(DynamicOverrideRule &rule,
                                           DynamicOverrideRuleProperty *existing_property);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Runtime/depgraph building & evaluation context.
 * \{ */

class DynamicOverrideDepsgraphCtx {
  Scene *scene_;
  ViewLayer *layer_;

  /**
   * All ID targets, i.e. all overridden IDs according to the active DynamicOverride
   * data-blocks in the evaluated scene & view layer.
   */
  Set<ID *> id_targets_ = {};
  bool id_targets_are_gathered_ = false;

  /**
   * All active DynamicOverride ID targets, i.e. all overridden IDs according to the active
   * DynamicOverride data-blocks in the evaluated scene & view layer.
   */
  VectorSet<DynamicOverride *> dynamic_overrides_ = {};
  bool dynamic_overrides_are_gathered_ = false;

 public:
  DynamicOverrideDepsgraphCtx(Scene *scene, ViewLayer *layer) : scene_(scene), layer_(layer) {}
  virtual ~DynamicOverrideDepsgraphCtx() = default;

  bool has_overrides() const
  {
    return !dynamic_overrides_.is_empty();
  }

  /** Reset internal data, clear any cached/evaluated override info. */
  void reset_data(Scene *scene, ViewLayer *layer)
  {
    scene_ = scene;
    layer_ = layer;
    id_targets_.clear();
    id_targets_are_gathered_ = false;
    dynamic_overrides_.clear();
    dynamic_overrides_are_gathered_ = false;
  }

  /**
   * Gather all active DynamicOverride IDs for the current scene & view layer.
   */
  void gather_dynamic_overrides();

  /**
   * Gather all affected (overridden) IDs from the list of active dynamic overrides.
   */
  void gather_id_targets();
};

/** \} */

}  // namespace bke

}  // namespace blender
