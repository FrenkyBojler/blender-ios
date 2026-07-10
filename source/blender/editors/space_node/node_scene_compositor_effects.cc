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
 * Add Effect Operator.
 */

static wmOperatorStatus add_scene_compositor_effect_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);
  bke::compositor::new_effect(*scene, "Scene Compositor Effect");
  WM_event_add_notifier(C, NC_SCENE | ND_COMPO_RESULT, scene);
  return OPERATOR_FINISHED;
}

void NODE_OT_add_scene_compositor_effect(wmOperatorType *ot)
{
  ot->name = "Add Scene Compositor Effect";
  ot->idname = "NODE_OT_add_scene_compositor_effect";
  ot->description = "Add a compositor effect to the scene";

  ot->exec = add_scene_compositor_effect_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/* --------------------------------------------------------------------
 * Remove Effect Operator.
 */

static wmOperatorStatus remove_scene_compositor_effect_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);

  const std::string name = RNA_string_get(op->ptr, "name");
  SceneCompositorEffect *effect = bke::compositor::get_effect(*scene, name);
  if (!effect) {
    return OPERATOR_CANCELLED;
  }

  bke::compositor::remove_effect(*scene, *effect);

  // TODO: Updates.
  WM_event_add_notifier(C, NC_SCENE | ND_COMPO_RESULT, scene);
  return OPERATOR_FINISHED;
}

void NODE_OT_remove_scene_compositor_effect(wmOperatorType *ot)
{
  PropertyRNA *prop;

  ot->name = "Remove Scene Compositor Effect";
  ot->idname = "NODE_OT_remove_scene_compositor_effect";
  ot->description = "Remove a effect from the strip";

  ot->exec = remove_scene_compositor_effect_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  prop = RNA_def_string(ot->srna, "name", "Name", MAX_NAME, "Name", "Name of effect to remove");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/* --------------------------------------------------------------------
 * Move Effect Operator.
 */

enum class EffectMoveDirection : uint8_t {
  Up,
  Down,
};

static wmOperatorStatus move_scene_compositor_effect_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);

  const std::string name = RNA_string_get(op->ptr, "name");
  EffectMoveDirection direction = EffectMoveDirection(RNA_enum_get(op->ptr, "direction"));

  SceneCompositorEffect *effect = bke::compositor::get_effect(*scene, name);
  if (!effect) {
    return OPERATOR_CANCELLED;
  }

  if (direction == EffectMoveDirection::Up) {
    if (effect->previous) {
      BLI_remlink(&scene->compositor_effects, effect);
      BLI_insertlinkbefore(&scene->compositor_effects, effect->previous, effect);
    }
  }
  else if (direction == EffectMoveDirection::Up) {
    if (effect->next) {
      BLI_remlink(&scene->compositor_effects, effect);
      BLI_insertlinkafter(&scene->compositor_effects, effect->next, effect);
    }
  }
  else {
    return OPERATOR_CANCELLED;
  }

  // TODO: Updates.
  WM_event_add_notifier(C, NC_SCENE | ND_COMPO_RESULT, scene);
  return OPERATOR_FINISHED;
}

void NODE_OT_move_scene_compositor_effect(wmOperatorType *ot)
{
  static const EnumPropertyItem direction_items[] = {
      {int(EffectMoveDirection::Up), "UP", 0, "Up", "Move effect up in the stack"},
      {int(EffectMoveDirection::Down), "DOWN", 0, "Down", "Move effect down in the stack"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Move Scene Compositor Effect";
  ot->idname = "NODE_OT_move_scene_compositor_effect";
  ot->description = "Move effect up and down in the stack";

  ot->exec = move_scene_compositor_effect_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna, "name", "Name", MAX_NAME, "Name", "Name of effect to move");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  prop = RNA_def_enum(
      ot->srna, "direction", direction_items, int(EffectMoveDirection::Up), "Direction", "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/* --------------------------------------------------------------------
 * Duplicate Effect Operator.
 */

static wmOperatorStatus duplicate_scene_compositor_effect_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  if (scene->compositor_effects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  std::string name = RNA_string_get(op->ptr, "name");
  SceneCompositorEffect *effect = name.empty() ? bke::compositor::get_active_effect(*scene) :
                                                 bke::compositor::get_effect(*scene, name);
  if (!effect) {
    return OPERATOR_CANCELLED;
  }

  bke::compositor::copy_effect(*scene, *effect);

  // TODO: Updates.
  WM_event_add_notifier(C, NC_SCENE | ND_COMPO_RESULT, scene);
  return OPERATOR_FINISHED;
}

void NODE_OT_duplicate_scene_compositor_effect(wmOperatorType *ot)
{
  ot->name = "Duplicate Scene Compositor Effect";
  ot->idname = "NODE_OT_duplicate_scene_compositor_effect";
  ot->description = "Duplicate the active or the given effect";

  ot->exec = duplicate_scene_compositor_effect_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_string(
      ot->srna,
      "name",
      nullptr,
      MAX_NAME,
      "Name",
      "Name of the effect to duplicate. If empty duplicate the active effect");
  RNA_def_property_flag(ot->prop, PROP_HIDDEN);
}

/* --------------------------------------------------------------------
 * Move Effect To Index Operator.
 */

static wmOperatorStatus move_scene_compositor_effect_to_index_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  const std::string name = RNA_string_get(op->ptr, "name");
  SceneCompositorEffect *effect = bke::compositor::get_effect(*scene, name);
  if (!effect) {
    return OPERATOR_CANCELLED;
  }

  const int current_index = BLI_findindex(&scene->compositor_effects, effect);
  const int new_index = RNA_int_get(op->ptr, "index");
  const bool successful = BLI_listbase_move_index(
      &scene->compositor_effects, current_index, new_index);
  if (!successful) {
    return OPERATOR_CANCELLED;
  }

  // TODO: Updates.
  WM_event_add_notifier(C, NC_SCENE | ND_COMPO_RESULT, scene);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus move_scene_compositor_effect_to_index_invoke(bContext *C,
                                                                     wmOperator *op,
                                                                     const wmEvent * /*event*/)
{
  return move_scene_compositor_effect_to_index_exec(C, op);
}

void NODE_OT_scene_compositor_effect_move_to_index(wmOperatorType *ot)
{
  ot->name = "Move Active Scene Compositor Effect to Index";
  ot->description =
      "Change the scene compositor effect's index in the stack so it evaluates after the set "
      "number of others";
  ot->idname = "NODE_OT_scene_compositor_effect_move_to_index";

  ot->invoke = move_scene_compositor_effect_to_index_invoke;
  ot->exec = move_scene_compositor_effect_to_index_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the effect to edit");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  RNA_def_int(
      ot->srna, "index", 0, 0, INT_MAX, "Index", "The index to move the effect to", 0, INT_MAX);
}

/* --------------------------------------------------------------------
 * Set Active Effect Operator.
 */

static wmOperatorStatus set_active_scene_compositor_effect_exec(bContext *C, wmOperator *op)
{
  Scene *scene = CTX_data_scene(C);
  const std::string name = RNA_string_get(op->ptr, "name");
  SceneCompositorEffect *effect = bke::compositor::get_effect(*scene, name);
  if (!effect) {
    return OPERATOR_CANCELLED;
  }
  bke::compositor::set_active_effect(*scene, *effect);

  // TODO: Updates.
  WM_event_add_notifier(C, NC_SCENE | ND_COMPO_RESULT, scene);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus set_active_scene_compositor_effect_invoke(bContext *C,
                                                                  wmOperator *op,
                                                                  const wmEvent * /*event*/)
{
  return set_active_scene_compositor_effect_exec(C, op);
}

void NODE_OT_set_active_scene_compositor_effect(wmOperatorType *ot)
{
  ot->name = "Set Active Scene Compositor Effect";
  ot->description = "Set the given scene compositor effect as the active one";
  ot->idname = "NODE_OT_set_active_scene_compositor_effect";

  ot->invoke = set_active_scene_compositor_effect_invoke;
  ot->exec = set_active_scene_compositor_effect_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  ot->prop = RNA_def_string(
      ot->srna, "name", nullptr, MAX_NAME, "Name", "Name of the strip effect to edit");
  RNA_def_property_flag(ot->prop, PROP_HIDDEN);
}

}  // namespace blender::ed::space_node
