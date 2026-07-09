/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 */

#include "BLI_listbase.hh"

#include "DNA_scene_types.h"

#include "DEG_depsgraph.hh"

#include "BKE_compositor.hh"
#include "BKE_context.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "UI_interface_c.hh"

namespace blender::ed::space_node {

/* --------------------------------------------------------------------
 * Add Modifier Operator.
 */

static wmOperatorStatus add_scene_compositor_modifier_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);
  bke::compositor::new_modifier(*scene, "Scene Compositor Modifier");
  WM_event_add_notifier(C, NC_SCENE | ND_MODIFIER, scene);
  return OPERATOR_FINISHED;
}

void NODE_OT_add_scene_compositor_modifier(wmOperatorType *operator_type)
{
  operator_type->name = "Add Scene Compositor Modifier";
  operator_type->idname = "NODE_OT_add_scene_compositor_modifier";
  operator_type->description = "Add a compositor modifier to the scene";

  operator_type->exec = add_scene_compositor_modifier_exec;

  operator_type->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* --------------------------------------------------------------------
 * Remove Modifier Operator.
 */

static wmOperatorStatus remove_scene_compositor_modifier_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);

  const std::string name = RNA_string_get(op->ptr, "name");
  SceneCompositorModifier *modifier = bke::compositor::get_modifier(*scene, name);
  if (!modifier) {
    return OPERATOR_CANCELLED;
  }

  bke::compositor::remove_modifier(*scene, *modifier);

  // TODO: Updates.
  WM_event_add_notifier(C, NC_SCENE | ND_MODIFIER, scene);
  return OPERATOR_FINISHED;
}

void NODE_OT_remove_scene_compositor_modifier(wmOperatorType *operator_type)
{
  PropertyRNA *prop;

  operator_type->name = "Remove Scene Compositor Modifier";
  operator_type->idname = "NODE_OT_remove_scene_compositor_modifier";
  operator_type->description = "Remove a modifier from the strip";

  operator_type->exec = remove_scene_compositor_modifier_exec;

  operator_type->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_string(
      operator_type->srna, "name", "Name", MAX_NAME, "Name", "Name of modifier to remove");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/* --------------------------------------------------------------------
 * Move Modifier Operator.
 */

enum class ModifierMoveDirection : uint8_t {
  Up,
  Down,
};

static wmOperatorStatus move_scene_compositor_modifier_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);

  const std::string name = RNA_string_get(op->ptr, "name");
  ModifierMoveDirection direction = ModifierMoveDirection(RNA_enum_get(op->ptr, "direction"));

  SceneCompositorModifier *modifier = bke::compositor::get_modifier(*scene, name);
  if (!modifier) {
    return OPERATOR_CANCELLED;
  }

  if (direction == ModifierMoveDirection::Up) {
    if (modifier->previous) {
      BLI_remlink(&scene->compositor_modifiers, modifier);
      BLI_insertlinkbefore(&scene->compositor_modifiers, modifier->previous, modifier);
    }
  }
  else if (direction == ModifierMoveDirection::Up) {
    if (modifier->next) {
      BLI_remlink(&scene->compositor_modifiers, modifier);
      BLI_insertlinkafter(&scene->compositor_modifiers, modifier->next, modifier);
    }
  }
  else {
    return OPERATOR_CANCELLED;
  }

  // TODO: Updates.
  WM_event_add_notifier(C, NC_SCENE | ND_MODIFIER, scene);
  return OPERATOR_FINISHED;
}

void NODE_OT_move_scene_compositor_modifier(wmOperatorType *operator_type)
{
  static const EnumPropertyItem direction_items[] = {
      {int(ModifierMoveDirection::Up), "UP", 0, "Up", "Move modifier up in the stack"},
      {int(ModifierMoveDirection::Down), "DOWN", 0, "Down", "Move modifier down in the stack"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  operator_type->name = "Move Scene Compositor Modifier";
  operator_type->idname = "NODE_OT_move_scene_compositor_modifier";
  operator_type->description = "Move modifier up and down in the stack";

  operator_type->exec = move_scene_compositor_modifier_exec;

  operator_type->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;
  prop = RNA_def_string(
      operator_type->srna, "name", "Name", MAX_NAME, "Name", "Name of modifier to move");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_enum(operator_type->srna,
                      "direction",
                      direction_items,
                      int(ModifierMoveDirection::Up),
                      "Direction",
                      "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/* --------------------------------------------------------------------
 * Duplicate Modifier Operator.
 */

static wmOperatorStatus duplicate_scene_compositor_modifier_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  if (scene->compositor_modifiers.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  std::string name = RNA_string_get(op->ptr, "name");
  SceneCompositorModifier *modifier = name.empty() ? bke::compositor::get_active_modifier(*scene) :
                                                     bke::compositor::get_modifier(*scene, name);
  if (!modifier) {
    return OPERATOR_CANCELLED;
  }

  bke::compositor::copy_modifier(*scene, *modifier);

  // TODO: Updates.
  WM_event_add_notifier(C, NC_SCENE | ND_MODIFIER, scene);
  return OPERATOR_FINISHED;
}

void NODE_OT_duplicate_scene_compositor_modifier(wmOperatorType *operator_type)
{
  operator_type->name = "Duplicate Scene Compositor Modifier";
  operator_type->idname = "NODE_OT_duplicate_scene_compositor_modifier";
  operator_type->description = "Duplicate the active or the given modifier";

  operator_type->exec = duplicate_scene_compositor_modifier_exec;

  operator_type->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  operator_type->prop = RNA_def_string(
      operator_type->srna,
      "name",
      nullptr,
      MAX_NAME,
      "Name",
      "Name of the modifier to duplicate. If empty duplicate the active modifier");
  RNA_def_property_flag(operator_type->prop, PROP_HIDDEN);
}

/* --------------------------------------------------------------------
 * Move Modifier To Index Operator.
 */

static wmOperatorStatus move_scene_compositor_modifier_to_index_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  const std::string name = RNA_string_get(op->ptr, "name");
  SceneCompositorModifier *modifier = bke::compositor::get_modifier(*scene, name);
  if (!modifier) {
    return OPERATOR_CANCELLED;
  }

  const int current_index = BLI_findindex(&scene->compositor_modifiers, modifier);
  const int new_index = RNA_int_get(op->ptr, "index");
  const bool successful = BLI_listbase_move_index(
      &scene->compositor_modifiers, current_index, new_index);
  if (!successful) {
    return OPERATOR_CANCELLED;
  }

  // TODO: Updates.
  WM_event_add_notifier(C, NC_SCENE | ND_MODIFIER, scene);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus move_scene_compositor_modifier_to_index_invoke(bContext *C,
                                                                       wmOperator *op,
                                                                       const wmEvent * /*event*/)
{
  return move_scene_compositor_modifier_to_index_exec(C, op);
}

void NODE_OT_scene_compositor_modifier_move_to_index(wmOperatorType *operator_type)
{
  operator_type->name = "Move Active Scene Compositor Modifier to Index";
  operator_type->description =
      "Change the scene compositor modifier's index in the stack so it evaluates after the set "
      "number of others";
  operator_type->idname = "NODE_OT_scene_compositor_modifier_move_to_index";

  operator_type->invoke = move_scene_compositor_modifier_to_index_invoke;
  operator_type->exec = move_scene_compositor_modifier_to_index_exec;

  operator_type->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  PropertyRNA *prop;
  prop = RNA_def_string(
      operator_type->srna, "name", nullptr, MAX_NAME, "Name", "Name of the modifier to edit");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  RNA_def_int(operator_type->srna,
              "index",
              0,
              0,
              INT_MAX,
              "Index",
              "The index to move the modifier to",
              0,
              INT_MAX);
}

/* --------------------------------------------------------------------
 * Set Active Modifier Operator.
 */

static wmOperatorStatus set_active_scene_compositor_modifier_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  const std::string name = RNA_string_get(op->ptr, "name");
  SceneCompositorModifier *modifier = bke::compositor::get_modifier(*scene, name);
  if (!modifier) {
    return OPERATOR_CANCELLED;
  }
  bke::compositor::set_active_modifier(*scene, *modifier);

  // TODO: Updates.
  WM_event_add_notifier(C, NC_SCENE | ND_MODIFIER, scene);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus set_active_scene_compositor_modifier_invoke(bContext *C,
                                                                    wmOperator *op,
                                                                    const wmEvent * /*event*/)
{
  return set_active_scene_compositor_modifier_exec(C, op);
}

void NODE_OT_set_active_scene_compositor_modifier(wmOperatorType *operator_type)
{
  operator_type->name = "Set Active Scene Compositor Modifier";
  operator_type->description = "Set the given scene compositor modifier as the active one";
  operator_type->idname = "NODE_OT_set_active_scene_compositor_modifier";

  operator_type->invoke = set_active_scene_compositor_modifier_invoke;
  operator_type->exec = set_active_scene_compositor_modifier_exec;

  operator_type->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  operator_type->prop = RNA_def_string(operator_type->srna,
                                       "name",
                                       nullptr,
                                       MAX_NAME,
                                       "Name",
                                       "Name of the strip modifier to edit");
  RNA_def_property_flag(operator_type->prop, PROP_HIDDEN);
}

}  // namespace blender::ed::space_node
