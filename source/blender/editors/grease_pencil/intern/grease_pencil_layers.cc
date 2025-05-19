/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edgreasepencil
 */

#include "BLI_listbase.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_string.h"

#include "BKE_attribute_math.hh"
#include "BKE_context.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "ED_grease_pencil.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"

#include "DNA_scene_types.h"

#include "WM_api.hh"
#include "WM_message.hh"

namespace blender::ed::greasepencil {

/* This utility function is modified from `BKE_object_get_parent_matrix()`. */
static void get_bone_mat(const Object *parent, const char *parsubstr, float4x4 &r_mat)
{
  if (parent->type != OB_ARMATURE) {
    r_mat = float4x4::identity();
    return;
  }

  const bPoseChannel *pchan = BKE_pose_channel_find_name(parent->pose, parsubstr);
  if (!pchan || !pchan->bone) {
    r_mat = float4x4::identity();
    return;
  }

  if (pchan->bone->flag & BONE_RELATIVE_PARENTING) {
    r_mat = float4x4(pchan->chan_mat);
  }
  else {
    r_mat = float4x4(pchan->pose_mat);
  }
}

bool grease_pencil_layer_parent_set(bke::greasepencil::Layer &layer,
                                    Object *parent,
                                    StringRefNull bone,
                                    const bool keep_transform)
{
  if (keep_transform) {
    /* TODO apply current transform to geometry. */
  }

  layer.parent = parent;
  layer.parsubstr = BLI_strdup_null(bone.c_str());
  /* Calculate inverse parent matrix. */
  if (parent) {
    copy_m4_m4(layer.parentinv, parent->world_to_object().ptr());
    if (layer.parsubstr) {
      float4x4 bone_mat;
      get_bone_mat(parent, layer.parsubstr, bone_mat);
      float4x4 bone_inverse = math::invert(bone_mat) * float4x4(layer.parentinv);
      copy_m4_m4(layer.parentinv, bone_inverse.ptr());
    }
  }
  else {
    unit_m4(layer.parentinv);
  }

  return true;
}

void grease_pencil_layer_parent_clear(bke::greasepencil::Layer &layer, const bool keep_transform)
{
  if (layer.parent == nullptr) {
    return;
  }
  if (keep_transform) {
    /* TODO apply current transform to geometry. */
  }

  layer.parent = nullptr;
  MEM_SAFE_FREE(layer.parsubstr);

  copy_m4_m4(layer.parentinv, float4x4::identity().ptr());
}

void select_layer_channel(GreasePencil &grease_pencil, bke::greasepencil::Layer *layer)
{
  using namespace blender::bke::greasepencil;

  if (layer != nullptr) {
    layer->set_selected(true);
  }

  if (grease_pencil.get_active_layer() != layer) {
    grease_pencil.set_active_layer(layer);
    WM_main_add_notifier(NC_GPENCIL | ND_DATA | NA_EDITED, &grease_pencil);
  }
}

static wmOperatorStatus grease_pencil_layer_add_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  Scene *scene = CTX_data_scene(C);
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);

  int new_layer_name_length;
  char *new_layer_name = RNA_string_get_alloc(
      op->ptr, "new_layer_name", nullptr, 0, &new_layer_name_length);
  BLI_SCOPED_DEFER([&] { MEM_SAFE_FREE(new_layer_name); });
  Layer &new_layer = grease_pencil.add_layer(new_layer_name);
  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3, layers);

  if (grease_pencil.has_active_layer()) {
    grease_pencil.move_node_after(new_layer.as_node(),
                                  grease_pencil.get_active_layer()->as_node());
  }
  else if (grease_pencil.has_active_group()) {
    grease_pencil.move_node_into(new_layer.as_node(), *grease_pencil.get_active_group());
    WM_msg_publish_rna_prop(CTX_wm_message_bus(C),
                            &grease_pencil.id,
                            &grease_pencil,
                            GreasePencilv3LayerGroup,
                            active);
  }

  grease_pencil.set_active_layer(&new_layer);
  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3Layers, active);

  grease_pencil.insert_frame(new_layer, scene->r.cfra);

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus grease_pencil_layer_add_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  return WM_operator_props_popup_confirm_ex(C,
                                            op,
                                            event,
                                            IFACE_("Add New Grease Pencil Layer"),
                                            CTX_IFACE_(BLT_I18NCONTEXT_OPERATOR_DEFAULT, "Add"));
}

static void GREASE_PENCIL_OT_layer_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add New Layer";
  ot->idname = "GREASE_PENCIL_OT_layer_add";
  ot->description = "Add a new Grease Pencil layer in the active object";

  /* callbacks */
  ot->invoke = grease_pencil_layer_add_invoke;
  ot->exec = grease_pencil_layer_add_exec;
  ot->poll = grease_pencil_context_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_string(
      ot->srna, "new_layer_name", "Layer", INT16_MAX, "Name", "Name of the new layer");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;
}

static wmOperatorStatus grease_pencil_layer_remove_exec(bContext *C, wmOperator * /*op*/)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);

  if (!grease_pencil.has_active_layer()) {
    return OPERATOR_CANCELLED;
  }

  grease_pencil.remove_layer(*grease_pencil.get_active_layer());

  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3Layers, active);
  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3, layers);

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Layer";
  ot->idname = "GREASE_PENCIL_OT_layer_remove";
  ot->description = "Remove the active Grease Pencil layer";

  /* callbacks */
  ot->exec = grease_pencil_layer_remove_exec;
  ot->poll = active_grease_pencil_layer_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

enum class LayerMoveDirection : int8_t { Up = -1, Down = 1 };

static const EnumPropertyItem enum_layer_move_direction[] = {
    {int(LayerMoveDirection::Up), "UP", 0, "Up", ""},
    {int(LayerMoveDirection::Down), "DOWN", 0, "Down", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static bool grease_pencil_layer_move_poll(bContext *C)
{
  using namespace blender::bke::greasepencil;
  if (!grease_pencil_context_poll(C)) {
    return false;
  }

  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);
  const TreeNode *active_node = grease_pencil.get_active_node();

  if (active_node == nullptr) {
    return false;
  }

  const LayerGroup *parent = active_node->parent_group();

  if (parent == nullptr || parent->num_direct_nodes() < 2) {
    return false;
  }

  return true;
}

static wmOperatorStatus grease_pencil_layer_move_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);

  const LayerMoveDirection direction = LayerMoveDirection(RNA_enum_get(op->ptr, "direction"));

  TreeNode &active_node = *grease_pencil.get_active_node();

  if (direction == LayerMoveDirection::Up) {
    grease_pencil.move_node_up(active_node);
  }
  else if (direction == LayerMoveDirection::Down) {
    grease_pencil.move_node_down(active_node);
  }

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);

  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3, layers);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_move(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reorder Layer";
  ot->idname = "GREASE_PENCIL_OT_layer_move";
  ot->description = "Move the active Grease Pencil layer or Group";

  /* callbacks */
  ot->exec = grease_pencil_layer_move_exec;
  ot->poll = grease_pencil_layer_move_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna, "direction", enum_layer_move_direction, 0, "Direction", "");
}

static wmOperatorStatus grease_pencil_layer_active_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  int layer_index = RNA_int_get(op->ptr, "layer");

  if (!grease_pencil.layers().index_range().contains(layer_index)) {
    return OPERATOR_CANCELLED;
  }

  Layer &layer = grease_pencil.layer(layer_index);
  if (grease_pencil.is_layer_active(&layer)) {
    return OPERATOR_CANCELLED;
  }

  if (grease_pencil.has_active_group()) {
    WM_msg_publish_rna_prop(CTX_wm_message_bus(C),
                            &grease_pencil.id,
                            &grease_pencil,
                            GreasePencilv3LayerGroup,
                            active);
  }
  grease_pencil.set_active_layer(&layer);

  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3Layers, active);

  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_active(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Set Active Layer";
  ot->idname = "GREASE_PENCIL_OT_layer_active";
  ot->description = "Set the active Grease Pencil layer";

  /* callbacks */
  ot->exec = grease_pencil_layer_active_exec;
  ot->poll = active_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_int(
      ot->srna, "layer", 0, 0, INT_MAX, "Grease Pencil Layer", "", 0, INT_MAX);
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
}

static wmOperatorStatus grease_pencil_layer_group_add_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);

  int new_layer_group_name_length;
  char *new_layer_group_name = RNA_string_get_alloc(
      op->ptr, "new_layer_group_name", nullptr, 0, &new_layer_group_name_length);

  LayerGroup &new_group = grease_pencil.add_layer_group(new_layer_group_name);
  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3, layer_groups);

  if (grease_pencil.has_active_layer()) {
    grease_pencil.move_node_after(new_group.as_node(),
                                  grease_pencil.get_active_layer()->as_node());
    WM_msg_publish_rna_prop(
        CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3Layers, active);
  }
  else if (grease_pencil.has_active_group()) {
    grease_pencil.move_node_into(new_group.as_node(), *grease_pencil.get_active_group());
    WM_msg_publish_rna_prop(CTX_wm_message_bus(C),
                            &grease_pencil.id,
                            &grease_pencil,
                            GreasePencilv3LayerGroup,
                            active);
  }

  MEM_SAFE_FREE(new_layer_group_name);
  grease_pencil.set_active_node(&new_group.as_node());

  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3LayerGroup, active);

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_group_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add New Layer Group";
  ot->idname = "GREASE_PENCIL_OT_layer_group_add";
  ot->description = "Add a new Grease Pencil layer group in the active object";

  /* callbacks */
  ot->exec = grease_pencil_layer_group_add_exec;
  ot->poll = grease_pencil_context_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop = RNA_def_string(
      ot->srna, "new_layer_group_name", nullptr, INT16_MAX, "Name", "Name of the new layer group");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;
}

static wmOperatorStatus grease_pencil_layer_group_remove_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  const bool keep_children = RNA_boolean_get(op->ptr, "keep_children");
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);

  if (!grease_pencil.has_active_group()) {
    return OPERATOR_CANCELLED;
  }

  grease_pencil.remove_group(*grease_pencil.get_active_group(), keep_children);

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, &grease_pencil);

  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3LayerGroup, active);
  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3, layer_groups);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_group_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Layer Group";
  ot->idname = "GREASE_PENCIL_OT_layer_group_remove";
  ot->description = "Remove Grease Pencil layer group in the active object";

  /* callbacks */
  ot->exec = grease_pencil_layer_group_remove_exec;
  ot->poll = grease_pencil_context_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "keep_children",
                  false,
                  "Keep children nodes",
                  "Keep the children nodes of the group and only delete the group itself");
}

static wmOperatorStatus grease_pencil_layer_hide_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);
  const bool unselected = RNA_boolean_get(op->ptr, "unselected");

  TreeNode *active_node = grease_pencil.get_active_node();

  if (!active_node) {
    return OPERATOR_CANCELLED;
  }

  if (unselected) {
    /* If active node is a layer group, only show parent layer groups and child nodes.
     *  If active node is a layer, only show parent layer groups and active node. */

    for (TreeNode *node : grease_pencil.nodes_for_write()) {
      bool should_be_visible = false;

      if (active_node->is_group()) {
        should_be_visible = node->is_child_of(active_node->as_group());
        if (node->is_group()) {
          should_be_visible |= active_node->is_child_of(node->as_group());
        }
      }
      else if (node->is_group()) {
        should_be_visible = active_node->is_child_of(node->as_group());
      }

      node->set_visible(should_be_visible);
    }
    active_node->set_visible(true);
  }
  else {
    /* hide selected/active */
    active_node->set_visible(false);
  }

  /* notifiers */
  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_hide(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Hide Layer(s)";
  ot->idname = "GREASE_PENCIL_OT_layer_hide";
  ot->description = "Hide selected/unselected Grease Pencil layers";

  /* callbacks */
  ot->exec = grease_pencil_layer_hide_exec;
  ot->poll = grease_pencil_context_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* props */
  PropertyRNA *prop = RNA_def_boolean(
      ot->srna, "unselected", false, "Unselected", "Hide unselected rather than selected layers");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
  ot->prop = prop;
}

static wmOperatorStatus grease_pencil_layer_reveal_exec(bContext *C, wmOperator * /*op*/)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);

  if (!grease_pencil.get_active_node()) {
    return OPERATOR_CANCELLED;
  }

  for (TreeNode *node : grease_pencil.nodes_for_write()) {
    node->set_visible(true);
  }

  /* notifiers */
  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_reveal(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Show All Layers";
  ot->idname = "GREASE_PENCIL_OT_layer_reveal";
  ot->description = "Show all Grease Pencil layers";

  /* callbacks */
  ot->exec = grease_pencil_layer_reveal_exec;
  ot->poll = grease_pencil_context_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus grease_pencil_layer_isolate_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);
  const int affect_visibility = RNA_boolean_get(op->ptr, "affect_visibility");
  bool isolate = false;

  for (const Layer *layer : grease_pencil.layers()) {
    if (grease_pencil.is_layer_active(layer)) {
      continue;
    }
    if ((affect_visibility && layer->is_visible()) || !layer->is_locked()) {
      isolate = true;
      break;
    }
  }

  for (Layer *layer : grease_pencil.layers_for_write()) {
    if (grease_pencil.is_layer_active(layer) || !isolate) {
      layer->set_locked(false);
      if (affect_visibility) {
        layer->set_visible(true);
      }
    }
    else {
      layer->set_locked(true);
      if (affect_visibility) {
        layer->set_visible(false);
      }
    }
  }

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_isolate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Isolate Layers";
  ot->idname = "GREASE_PENCIL_OT_layer_isolate";
  ot->description = "Make only active layer visible/editable";

  /* callbacks */
  ot->exec = grease_pencil_layer_isolate_exec;
  ot->poll = active_grease_pencil_layer_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(
      ot->srna, "affect_visibility", false, "Affect Visibility", "Also affect the visibility");
}

static wmOperatorStatus grease_pencil_layer_lock_all_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);
  const bool lock_value = RNA_boolean_get(op->ptr, "lock");

  if (grease_pencil.nodes().is_empty()) {
    return OPERATOR_CANCELLED;
  }

  for (TreeNode *node : grease_pencil.nodes_for_write()) {
    node->set_locked(lock_value);
  }

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, &grease_pencil);
  WM_event_add_notifier(C, NC_GPENCIL | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_lock_all(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Lock All Layers";
  ot->idname = "GREASE_PENCIL_OT_layer_lock_all";
  ot->description =
      "Lock all Grease Pencil layers to prevent them from being accidentally modified";

  /* callbacks */
  ot->exec = grease_pencil_layer_lock_all_exec;
  ot->poll = grease_pencil_context_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna, "lock", true, "Lock Value", "Lock/Unlock all layers");
}

static wmOperatorStatus grease_pencil_layer_duplicate_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);
  const bool empty_keyframes = RNA_boolean_get(op->ptr, "empty_keyframes");

  if (!grease_pencil.has_active_layer()) {
    BKE_reportf(op->reports, RPT_ERROR, "No active layer to duplicate");
    return OPERATOR_CANCELLED;
  }

  /* Duplicate layer. */
  Layer &active_layer = *grease_pencil.get_active_layer();
  Layer &new_layer = grease_pencil.duplicate_layer(active_layer);

  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3, layers);

  /* Clear source keyframes and recreate them with duplicated drawings. */
  new_layer.frames_for_write().clear();
  for (auto [frame_number, frame] : active_layer.frames().items()) {
    const int duration = active_layer.get_frame_duration_at(frame_number);

    Drawing *dst_drawing = grease_pencil.insert_frame(
        new_layer, frame_number, duration, eBezTriple_KeyframeType(frame.type));
    if (!empty_keyframes) {
      BLI_assert(dst_drawing != nullptr);
      /* TODO: This can fail (return `nullptr`) if the drawing is a drawing reference! */
      const Drawing &src_drawing = *grease_pencil.get_drawing_at(active_layer, frame_number);
      /* Duplicate the drawing. */
      *dst_drawing = src_drawing;
    }
  }

  grease_pencil.move_node_after(new_layer.as_node(), active_layer.as_node());
  grease_pencil.set_active_layer(&new_layer);

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, nullptr);

  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3Layers, active);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_duplicate(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Duplicate Layer";
  ot->idname = "GREASE_PENCIL_OT_layer_duplicate";
  ot->description = "Make a copy of the active Grease Pencil layer";

  /* callbacks */
  ot->exec = grease_pencil_layer_duplicate_exec;
  ot->poll = active_grease_pencil_layer_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  RNA_def_boolean(ot->srna, "empty_keyframes", false, "Empty Keyframes", "Add Empty Keyframes");
}

enum class MergeMode : int8_t {
  Down = 0,
  Group = 1,
  All = 2,
};

static wmOperatorStatus grease_pencil_merge_layer_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  Main *bmain = CTX_data_main(C);
  Object *object = CTX_data_active_object(C);
  GreasePencil &grease_pencil = *static_cast<GreasePencil *>(object->data);
  const MergeMode mode = MergeMode(RNA_enum_get(op->ptr, "mode"));

  Vector<Vector<int>> src_layer_indices_by_dst_layer;
  std::string merged_layer_name;
  if (mode == MergeMode::Down) {
    if (!grease_pencil.has_active_layer()) {
      BKE_report(op->reports, RPT_ERROR, "No active layer");
      return OPERATOR_CANCELLED;
    }
    const Layer &active_layer = *grease_pencil.get_active_layer();
    GreasePencilLayerTreeNode *prev_node = active_layer.as_node().prev;
    if (prev_node == nullptr || !prev_node->wrap().is_layer()) {
      /* No layer below the active one. */
      return OPERATOR_CANCELLED;
    }
    const Layer &prev_layer = prev_node->wrap().as_layer();
    /* Get the indices of the two layers to be merged. */
    const int prev_layer_index = *grease_pencil.get_layer_index(prev_layer);
    const int active_layer_index = *grease_pencil.get_layer_index(active_layer);

    /* Map all the other layers to their own index. */
    const Span<const Layer *> layers = grease_pencil.layers();
    for (const int layer_i : layers.index_range()) {
      if (layer_i == active_layer_index) {
        /* Active layer is merged into previous, skip. */
      }
      else if (layer_i == prev_layer_index) {
        /* Previous layer merges itself and the active layer. */
        src_layer_indices_by_dst_layer.append({prev_layer_index, active_layer_index});
      }
      else {
        /* Other layers remain unchanged. */
        src_layer_indices_by_dst_layer.append({layer_i});
      }
    }

    /* Store the name of the current active layer as the name of the merged layer. */
    merged_layer_name = grease_pencil.layer(prev_layer_index).name();
  }
  else if (mode == MergeMode::Group) {
    if (!grease_pencil.has_active_group()) {
      BKE_report(op->reports, RPT_ERROR, "No active group");
      return OPERATOR_CANCELLED;
    }
    LayerGroup &active_group = *grease_pencil.get_active_group();

    if (active_group.layers().is_empty()) {
      BKE_report(op->reports, RPT_INFO, "No child layers to merge");
      return OPERATOR_CANCELLED;
    }

    /* Remove all sub groups of the active group since they won't be needed anymore, but keep the
     * layers. */
    Array<LayerGroup *> groups = active_group.groups_for_write();
    for (LayerGroup *group : groups) {
      grease_pencil.remove_group(*group, true);
    }

    const Span<const Layer *> layers = grease_pencil.layers();
    Vector<int> indices;
    for (const int layer_i : layers.index_range()) {
      const Layer &layer = grease_pencil.layer(layer_i);
      if (!layer.is_child_of(active_group)) {
        src_layer_indices_by_dst_layer.append({layer_i});
      }
      else {
        indices.append(layer_i);
      }
    }
    src_layer_indices_by_dst_layer.append(indices);

    /* Store the name of the group as the name of the merged layer. */
    merged_layer_name = active_group.name();

    /* Remove the active group. */
    grease_pencil.remove_group(active_group, true);
    WM_msg_publish_rna_prop(CTX_wm_message_bus(C),
                            &grease_pencil.id,
                            &grease_pencil,
                            GreasePencilv3LayerGroup,
                            active);

    /* Rename the first node so that the merged layer will have the name of the group. */
    grease_pencil.rename_node(
        *bmain, grease_pencil.layer(indices[0]).as_node(), merged_layer_name);
  }
  else if (mode == MergeMode::All) {
    if (grease_pencil.layers().is_empty()) {
      return OPERATOR_CANCELLED;
    }
    /* Remove all groups, keep the layers. */
    Array<LayerGroup *> groups = grease_pencil.layer_groups_for_write();
    for (LayerGroup *group : groups) {
      grease_pencil.remove_group(*group, true);
    }

    Vector<int> indices;
    for (const int layer_i : grease_pencil.layers().index_range()) {
      indices.append(layer_i);
    }
    src_layer_indices_by_dst_layer.append(indices);

    merged_layer_name = N_("Layer");
    grease_pencil.rename_node(
        *bmain, grease_pencil.layer(indices[0]).as_node(), merged_layer_name);
  }
  else {
    BLI_assert_unreachable();
  }

  GreasePencil *merged_grease_pencil = BKE_grease_pencil_new_nomain();
  BKE_grease_pencil_copy_parameters(grease_pencil, *merged_grease_pencil);
  ed::greasepencil::merge_layers(
      grease_pencil, src_layer_indices_by_dst_layer, *merged_grease_pencil);
  BKE_grease_pencil_nomain_to_grease_pencil(merged_grease_pencil, &grease_pencil);

  WM_msg_publish_rna_prop(
      CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3, layers);

  /* Try to set the active (merged) layer. */
  TreeNode *node = grease_pencil.find_node_by_name(merged_layer_name);
  if (node && node->is_layer()) {
    Layer &layer = node->as_layer();
    grease_pencil.set_active_layer(&layer);

    WM_msg_publish_rna_prop(
        CTX_wm_message_bus(C), &grease_pencil.id, &grease_pencil, GreasePencilv3Layers, active);
  }

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, nullptr);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_merge(wmOperatorType *ot)
{
  static const EnumPropertyItem merge_modes[] = {
      {int(MergeMode::Down),
       "ACTIVE",
       0,
       "Active",
       "Combine the active layer with the layer just below (if it exists)"},
      {int(MergeMode::Group),
       "GROUP",
       0,
       "Group",
       "Combine layers in the active group into a single layer"},
      {int(MergeMode::All), "ALL", 0, "All", "Combine all layers into a single layer"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->name = "Merge";
  ot->idname = "GREASE_PENCIL_OT_layer_merge";
  ot->description = "Combine layers based on the mode into one layer";

  ot->exec = grease_pencil_merge_layer_exec;
  ot->poll = active_grease_pencil_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna, "mode", merge_modes, int(MergeMode::Down), "Mode", "");
}

static wmOperatorStatus grease_pencil_layer_mask_add_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);

  if (!grease_pencil.has_active_layer()) {
    return OPERATOR_CANCELLED;
  }
  Layer &active_layer = *grease_pencil.get_active_layer();

  int mask_name_length;
  char *mask_name = RNA_string_get_alloc(op->ptr, "name", nullptr, 0, &mask_name_length);
  BLI_SCOPED_DEFER([&] { MEM_SAFE_FREE(mask_name); });

  if (TreeNode *node = grease_pencil.find_node_by_name(mask_name)) {
    if (grease_pencil.is_layer_active(&node->as_layer())) {
      BKE_report(op->reports, RPT_ERROR, "Cannot add active layer as mask");
      return OPERATOR_CANCELLED;
    }

    if (BLI_findstring_ptr(&active_layer.masks,
                           mask_name,
                           offsetof(GreasePencilLayerMask, layer_name)) != nullptr)
    {
      BKE_report(op->reports, RPT_ERROR, "Layer already added");
      return OPERATOR_CANCELLED;
    }

    LayerMask *new_mask = MEM_new<LayerMask>(__func__, mask_name);
    BLI_addtail(&active_layer.masks, reinterpret_cast<GreasePencilLayerMask *>(new_mask));
    /* Make the newly added mask active. */
    active_layer.active_mask_index = BLI_listbase_count(&active_layer.masks) - 1;
  }
  else {
    BKE_report(op->reports, RPT_ERROR, "Unable to find layer to add");
    return OPERATOR_CANCELLED;
  }

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_mask_add(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Add New Mask Layer";
  ot->idname = "GREASE_PENCIL_OT_layer_mask_add";
  ot->description = "Add new layer as masking";

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* callbacks */
  ot->exec = grease_pencil_layer_mask_add_exec;
  ot->poll = active_grease_pencil_layer_poll;

  /* properties */
  RNA_def_string(ot->srna, "name", nullptr, 0, "Layer", "Name of the layer");
}

static bool grease_pencil_layer_mask_poll(bContext *C)
{
  using namespace blender::bke::greasepencil;
  if (!active_grease_pencil_layer_poll(C)) {
    return false;
  }

  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);
  Layer &active_layer = *grease_pencil.get_active_layer();

  return !BLI_listbase_is_empty(&active_layer.masks);
}

static wmOperatorStatus grease_pencil_layer_mask_remove_exec(bContext *C, wmOperator * /*op*/)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);

  if (!grease_pencil.has_active_layer()) {
    return OPERATOR_CANCELLED;
  }

  Layer &active_layer = *grease_pencil.get_active_layer();
  if (GreasePencilLayerMask *mask = reinterpret_cast<GreasePencilLayerMask *>(
          BLI_findlink(&active_layer.masks, active_layer.active_mask_index)))
  {
    BLI_remlink(&active_layer.masks, mask);
    MEM_delete(reinterpret_cast<LayerMask *>(mask));
    active_layer.active_mask_index = std::max(active_layer.active_mask_index - 1, 0);
  }
  else {
    return OPERATOR_CANCELLED;
  }

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_mask_remove(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Remove Mask Layer";
  ot->idname = "GREASE_PENCIL_OT_layer_mask_remove";
  ot->description = "Remove Layer Mask";

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* callbacks */
  ot->exec = grease_pencil_layer_mask_remove_exec;
  ot->poll = grease_pencil_layer_mask_poll;
}

static bool grease_pencil_layer_mask_reorder_poll(bContext *C)
{
  using namespace blender::bke::greasepencil;
  if (!active_grease_pencil_layer_poll(C)) {
    return false;
  }

  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);
  Layer &active_layer = *grease_pencil.get_active_layer();

  return BLI_listbase_count(&active_layer.masks) > 1;
}

static wmOperatorStatus grease_pencil_layer_mask_reorder_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);

  if (!grease_pencil.has_active_layer()) {
    return OPERATOR_CANCELLED;
  }
  Layer &active_layer = *grease_pencil.get_active_layer();
  const int direction = RNA_enum_get(op->ptr, "direction");

  bool changed = false;
  if (GreasePencilLayerMask *mask = reinterpret_cast<GreasePencilLayerMask *>(
          BLI_findlink(&active_layer.masks, active_layer.active_mask_index)))
  {
    if (BLI_listbase_link_move(&active_layer.masks, mask, direction)) {
      active_layer.active_mask_index = std::max(active_layer.active_mask_index + direction, 0);
      changed = true;
    }
  }
  else {
    return OPERATOR_CANCELLED;
  }

  if (changed) {
    DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA, &grease_pencil);
  }

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_mask_reorder(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Reorder Grease Pencil Layer Mask";
  ot->idname = "GREASE_PENCIL_OT_layer_mask_reorder";
  ot->description = "Reorder the active Grease Pencil mask layer up/down in the list";

  /* api callbacks */
  ot->exec = grease_pencil_layer_mask_reorder_exec;
  ot->poll = grease_pencil_layer_mask_reorder_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna, "direction", enum_layer_move_direction, 0, "Direction", "");
}

const EnumPropertyItem enum_layergroup_color_items[] = {
    {LAYERGROUP_COLOR_NONE, "NONE", ICON_X, "Set Default icon", ""},
    {LAYERGROUP_COLOR_01, "COLOR1", ICON_LAYERGROUP_COLOR_01, "Color tag 1", ""},
    {LAYERGROUP_COLOR_02, "COLOR2", ICON_LAYERGROUP_COLOR_02, "Color tag 2", ""},
    {LAYERGROUP_COLOR_03, "COLOR3", ICON_LAYERGROUP_COLOR_03, "Color tag 3", ""},
    {LAYERGROUP_COLOR_04, "COLOR4", ICON_LAYERGROUP_COLOR_04, "Color tag 4", ""},
    {LAYERGROUP_COLOR_05, "COLOR5", ICON_LAYERGROUP_COLOR_05, "Color tag 5", ""},
    {LAYERGROUP_COLOR_06, "COLOR6", ICON_LAYERGROUP_COLOR_06, "Color tag 6", ""},
    {LAYERGROUP_COLOR_07, "COLOR7", ICON_LAYERGROUP_COLOR_07, "Color tag 7", ""},
    {LAYERGROUP_COLOR_08, "COLOR8", ICON_LAYERGROUP_COLOR_08, "Color tag 8", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus grease_pencil_layer_group_color_tag_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);

  const int color_tag = RNA_enum_get(op->ptr, "color_tag");
  LayerGroup *active_group = grease_pencil.get_active_group();
  active_group->color_tag = color_tag;

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, &grease_pencil);

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_group_color_tag(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Grease Pencil Group Color Tag";
  ot->idname = "GREASE_PENCIL_OT_layer_group_color_tag";
  ot->description = "Change layer group icon";

  ot->exec = grease_pencil_layer_group_color_tag_exec;
  ot->poll = active_grease_pencil_layer_group_poll;

  ot->flag = OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna, "color_tag", enum_layergroup_color_items, 0, "Color Tag", "");
}

enum class DuplicateCopyMode {
  All = 0,
  Active,
};

static void duplicate_layer_and_frames(GreasePencil &dst_grease_pencil,
                                       const GreasePencil &src_grease_pencil,
                                       const blender::bke::greasepencil::Layer &src_layer,
                                       const DuplicateCopyMode copy_frame_mode,
                                       const int current_frame)
{
  using namespace blender::bke::greasepencil;

  if (&dst_grease_pencil == &src_grease_pencil) {
    /* Duplicating frames is valid if copying from the same object.
     * The resulting frames will reference existing drawings, which is more efficient than making
     * full copies. */
    Layer &dst_layer = dst_grease_pencil.duplicate_layer(src_layer);

    dst_layer.frames_for_write().clear();
    for (const auto [frame_number, frame] : src_layer.frames().items()) {
      if ((copy_frame_mode == DuplicateCopyMode::Active) &&
          (&frame != src_layer.frame_at(current_frame)))
      {
        continue;
      }
      const int duration = src_layer.get_frame_duration_at(frame_number);

      Drawing *dst_drawing = dst_grease_pencil.insert_frame(
          dst_layer, frame_number, duration, eBezTriple_KeyframeType(frame.type));
      if (dst_drawing != nullptr) {
        /* Duplicate drawing. */
        const Drawing &src_drawing = *src_grease_pencil.get_drawing_at(src_layer, frame_number);
        *dst_drawing = src_drawing;
      }
    }
  }
  else {
    /* When copying from another object a new layer is created and all drawings are copied. */
    const int src_layer_index = *src_grease_pencil.get_layer_index(src_layer);

    Layer &dst_layer = dst_grease_pencil.add_layer(src_layer.name());
    const int dst_layer_index = dst_grease_pencil.layers().size() - 1;

    BKE_grease_pencil_copy_layer_parameters(src_layer, dst_layer);

    const bke::AttributeAccessor src_attributes = src_grease_pencil.attributes();
    bke::MutableAttributeAccessor dst_attributes = dst_grease_pencil.attributes_for_write();
    src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
      if (iter.domain != bke::AttrDomain::Layer) {
        return;
      }
      bke::GAttributeReader reader = src_attributes.lookup(iter.name, iter.domain, iter.data_type);
      BLI_assert(reader);
      bke::GAttributeWriter writer = dst_attributes.lookup_or_add_for_write(
          iter.name, iter.domain, iter.data_type);
      if (writer) {
        const CPPType &cpptype = *bke::custom_data_type_to_cpp_type(iter.data_type);
        BUFFER_FOR_CPP_TYPE_VALUE(cpptype, buffer);
        reader.varray.get(src_layer_index, buffer);
        writer.varray.set_by_copy(dst_layer_index, buffer);
      }
      writer.finish();
    });

    std::optional<int> frame_select = std::nullopt;
    if (copy_frame_mode == DuplicateCopyMode::Active) {
      frame_select = current_frame;
    }
    dst_grease_pencil.copy_frames_from_layer(
        dst_layer, src_grease_pencil, src_layer, frame_select);
  }
}

static wmOperatorStatus grease_pencil_layer_duplicate_object_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  Object *src_object = CTX_data_active_object(C);
  const Scene *scene = CTX_data_scene(C);
  const int current_frame = scene->r.cfra;
  const GreasePencil &src_grease_pencil = *static_cast<GreasePencil *>(src_object->data);
  const bool only_active = RNA_boolean_get(op->ptr, "only_active");
  const DuplicateCopyMode copy_frame_mode = DuplicateCopyMode(RNA_enum_get(op->ptr, "mode"));

  CTX_DATA_BEGIN (C, Object *, ob, selected_objects) {
    if (ob == src_object || ob->type != OB_GREASE_PENCIL) {
      continue;
    }
    GreasePencil &dst_grease_pencil = *static_cast<GreasePencil *>(ob->data);

    if (only_active) {
      const Layer &active_layer = *src_grease_pencil.get_active_layer();
      duplicate_layer_and_frames(
          dst_grease_pencil, src_grease_pencil, active_layer, copy_frame_mode, current_frame);
    }
    else {
      for (const Layer *layer : src_grease_pencil.layers()) {
        duplicate_layer_and_frames(
            dst_grease_pencil, src_grease_pencil, *layer, copy_frame_mode, current_frame);
      }
    }

    DEG_id_tag_update(&dst_grease_pencil.id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_SELECTED, nullptr);
  }
  CTX_DATA_END;

  return OPERATOR_FINISHED;
}

static void GREASE_PENCIL_OT_layer_duplicate_object(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Duplicate Layer to New Object";
  ot->idname = "GREASE_PENCIL_OT_layer_duplicate_object";
  ot->description = "Make a copy of the active Grease Pencil layer to selected object";

  /* api callbacks */
  ot->poll = active_grease_pencil_layer_poll;
  ot->exec = grease_pencil_layer_duplicate_object_exec;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna,
                  "only_active",
                  true,
                  "Only Active",
                  "Copy only active Layer, uncheck to append all layers");

  static const EnumPropertyItem copy_mode[] = {
      {int(DuplicateCopyMode::All), "ALL", 0, "All Frames", ""},
      {int(DuplicateCopyMode::Active), "ACTIVE", 0, "Active Frame", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  ot->prop = RNA_def_enum(ot->srna, "mode", copy_mode, 0, "Mode", "");
}

using bke::greasepencil::LayerGroup;

typedef struct MoveToLayerGroupData {
  struct MoveToLayerGroupData *next, *prev;
  int index;
  LayerGroup *group;
  ListBase submenus;
  PointerRNA ptr;
  wmOperatorType *ot;
} MoveToLayerGroupData;

/* one static pointer to hold the menu tree for the active GPencil object */
static MoveToLayerGroupData *gp_layer_group_menu = NULL;

enum LayerGroupAction {
  LAYER_GROUP_ACTION_GROUP = 0,
  LAYER_GROUP_ACTION_UNGROUP = 1,
  LAYER_GROUP_ACTION_MOVE_INTO = 2,
};

static const EnumPropertyItem layer_group_action_items[] = {
    {LAYER_GROUP_ACTION_GROUP, "GROUP", 0, "Group", "Group selected layers into a new group"},
    {LAYER_GROUP_ACTION_UNGROUP, "UNGROUP", 0, "Ungroup", "Ungroup selected layers"},
    {LAYER_GROUP_ACTION_MOVE_INTO,
     "MOVE_INTO",
     0,
     "Move into Group",
     "Move selected layers into an existing group or create a new one"},
    {0, nullptr, 0, nullptr, nullptr},
};

static int move_to_layer_group_menus_create(wmOperator *op, MoveToLayerGroupData *menu)
{
  int index = menu->index;
  /* suppose menu->group->children is stored in a ListBase called 'children'. */
  ListBase *children = &menu->group->children;  // adjust field name as appropriate
  for (LinkData *link = (LinkData *)children->first; link; link = link->next) {
    LayerGroup *child = (LayerGroup *)link->data;
    MoveToLayerGroupData *submenu = MEM_new<MoveToLayerGroupData>("GPLayerGroupMenu");
    BLI_addtail(&menu->submenus, submenu);
    submenu->group = child;
    submenu->index = ++index;
    submenu->ot = op->type;
    index = move_to_layer_group_menus_create(op, submenu);
  }
  return index;
}

static void move_to_layer_group_menus_free_recursive(MoveToLayerGroupData *menu)
{
  LISTBASE_FOREACH_MUTABLE (MoveToLayerGroupData *, sub, &menu->submenus) {
    move_to_layer_group_menus_free_recursive(sub);
    MEM_delete(sub);
  }
  BLI_listbase_clear(&menu->submenus);
}

static void move_to_layer_group_menus_free(void)
{
  if (gp_layer_group_menu) {
    move_to_layer_group_menus_free_recursive(gp_layer_group_menu);
    MEM_delete(gp_layer_group_menu);
    gp_layer_group_menu = NULL;
  }
}

static void move_to_layer_group_menu_draw(bContext * /*C*/, uiLayout *layout, void *menu_v)
{
  MoveToLayerGroupData *menu = (MoveToLayerGroupData *)menu_v;
  /* Determine if an existing group is available */
  bool has_group = (menu->group != nullptr);

  WM_operator_properties_create_ptr(&menu->ptr, menu->ot);
  RNA_int_set(&menu->ptr, "target_group_index", menu->index);
  /* Mark as new if no existing group is associated */
  RNA_boolean_set(&menu->ptr, "is_new", !has_group);

  /* Always show the New Group operator */
  uiItemFullO_ptr(layout,
                  menu->ot,
                  IFACE_("New Group"),
                  ICON_ADD,
                  (IDProperty *)menu->ptr.data,
                  WM_OP_INVOKE_DEFAULT,
                  UI_ITEM_NONE,
                  NULL);
  uiItemS(layout);

  /* Only display the existing group option and any children if a group exists */
  if (has_group) {
    const char *name = menu->group->name().c_str();
    uiItemIntO(layout,
               name,
               ICON_OUTLINER_COLLECTION,
               menu->ot->idname,
               "target_group_index",
               menu->index);

    for (MoveToLayerGroupData *sub = (MoveToLayerGroupData *)menu->submenus.first; sub;
         sub = sub->next)
    {
      uiItemMenuF(
          layout, sub->group->name().c_str(), ICON_NONE, move_to_layer_group_menu_draw, sub);
    }
  }
}
static wmOperatorStatus grease_pencil_layer_group_manage_exec(bContext *C, wmOperator *op)
{
  using namespace blender::bke::greasepencil;
  GreasePencil &grease_pencil = *blender::ed::greasepencil::from_context(*C);
  int action = RNA_enum_get(op->ptr, "action");

  if (action == LAYER_GROUP_ACTION_MOVE_INTO) {
    if (!RNA_struct_property_is_set(op->ptr, "target_group_index")) {
      return OPERATOR_CANCELLED;
    }
    int target_group_index = RNA_int_get(op->ptr, "target_group_index");
    bool is_new = RNA_boolean_get(op->ptr, "is_new");
    if (is_new) {
      char new_group_name[MAX_NAME] = {0};
      RNA_string_get(op->ptr, "new_group_name", new_group_name);
      /* Provide a default name if none is given. */
      if (new_group_name[0] == '\0') {
        BLI_strncpy(new_group_name, "Group", sizeof(new_group_name));
      }
      LayerGroup *target_group = &grease_pencil.add_layer_group(new_group_name);
      Vector<Layer *> selected_layers;
      for (Layer *layer : grease_pencil.layers_for_write()) {
        if (layer->is_selected()) {
          selected_layers.append(layer);
        }
      }
      for (Layer *layer : selected_layers) {
        grease_pencil.move_node_into(layer->as_node(), *target_group);
      }
      grease_pencil.set_active_node(&target_group->as_node());
    }
    else {
      const Span<const LayerGroup *> cgroups = grease_pencil.layer_groups();
      if (!cgroups.index_range().contains(target_group_index)) {
        return OPERATOR_CANCELLED;
      }
      LayerGroup *target_group = const_cast<LayerGroup *>(cgroups[target_group_index]);
      Vector<Layer *> selected_layers;
      for (Layer *layer : grease_pencil.layers_for_write()) {
        if (layer->is_selected()) {
          selected_layers.append(layer);
        }
      }
      for (Layer *layer : selected_layers) {
        grease_pencil.move_node_into(layer->as_node(), *target_group);
      }
      grease_pencil.set_active_node(&target_group->as_node());
    }
  }
  else if (action == LAYER_GROUP_ACTION_GROUP) {
    /* Group: create a new group and add selected layers into it.
       If the first selected layer already belongs to a group (its parent name is non-empty),
       nest the new group in that parent. */
    Vector<Layer *> selected_layers;
    for (Layer *layer : grease_pencil.layers_for_write()) {
      if (layer->is_selected()) {
        selected_layers.append(layer);
      }
    }
    if (selected_layers.is_empty()) {
      return OPERATOR_CANCELLED;
    }
    const LayerGroup &first_parent = selected_layers[0]->parent_group();
    LayerGroup *new_group_ptr = nullptr;
    if (first_parent.name().size() != 0) {
      new_group_ptr = &grease_pencil.add_layer_group("Group");
      /* Nest the new group into the first parent's group */
      grease_pencil.move_node_into(new_group_ptr->as_node(),
                                   const_cast<LayerGroup &>(first_parent));
    }
    else {
      new_group_ptr = &grease_pencil.add_layer_group("Group");
    }
    for (Layer *layer : selected_layers) {
      grease_pencil.move_node_into(layer->as_node(), *new_group_ptr);
    }
    grease_pencil.set_active_node(&new_group_ptr->as_node());
  }
  else if (action == LAYER_GROUP_ACTION_MOVE_INTO) {
    /* MOVE_INTO: expect either a target group (by index) or a flag to create a new one.
       If the target group is not set, the invoke callback will pop up a dialog. */
    if (!RNA_struct_property_is_set(op->ptr, "target_group_index")) {
      return WM_operator_props_dialog_popup(C, op, 200, "Move to Group", "Move");
    }
    int target_group_index = RNA_int_get(op->ptr, "target_group_index");
    bool is_new = RNA_boolean_get(op->ptr, "is_new");
    if (is_new) {
      char new_group_name[MAX_NAME];
      RNA_string_get(op->ptr, "new_group_name", new_group_name);
      LayerGroup *target_group = &grease_pencil.add_layer_group(new_group_name);
      Vector<Layer *> selected_layers;
      for (Layer *layer : grease_pencil.layers_for_write()) {
        if (layer->is_selected()) {
          selected_layers.append(layer);
        }
      }
      for (Layer *layer : selected_layers) {
        grease_pencil.move_node_into(layer->as_node(), *target_group);
      }
      grease_pencil.set_active_node(&target_group->as_node());
    }
    else {
      const Span<const LayerGroup *> cgroups = grease_pencil.layer_groups();
      if (!cgroups.index_range().contains(target_group_index)) {
        return OPERATOR_CANCELLED;
      }
      LayerGroup *target_group = const_cast<LayerGroup *>(cgroups[target_group_index]);
      Vector<Layer *> selected_layers;
      for (Layer *layer : grease_pencil.layers_for_write()) {
        if (layer->is_selected()) {
          selected_layers.append(layer);
        }
      }
      for (Layer *layer : selected_layers) {
        grease_pencil.move_node_into(layer->as_node(), *target_group);
      }
      grease_pencil.set_active_node(&target_group->as_node());
    }
  }

  DEG_id_tag_update(&grease_pencil.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GPENCIL | ND_DATA | NA_EDITED, &grease_pencil);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus grease_pencil_layer_group_manage_invoke(bContext *C,
                                                                wmOperator *op,
                                                                const wmEvent * /*event*/)
{
  int action = RNA_enum_get(op->ptr, "action");
  if (action == LAYER_GROUP_ACTION_MOVE_INTO &&
      !RNA_struct_property_is_set(op->ptr, "target_group_index"))
  {
    GreasePencil *gpencil = blender::ed::greasepencil::from_context(*C);
    /* If no groups exist, force the 'new group' branch. */
    if (gpencil->layer_groups().is_empty()) {
      RNA_boolean_set(op->ptr, "is_new", true);
    }
    else {
      RNA_boolean_set(op->ptr, "is_new", false);
    }
    /* Free any existing menu tree. */
    move_to_layer_group_menus_free();
    /* Allocate the menu data before using it. */
    gp_layer_group_menu = MEM_new<MoveToLayerGroupData>("GP_LayerGroupMenu");
    if (!gpencil->layer_groups().is_empty()) {
      gp_layer_group_menu->group = const_cast<LayerGroup *>(gpencil->layer_groups()[0]);
    }
    else {
      gp_layer_group_menu->group = nullptr;
    }
    gp_layer_group_menu->index = 0;
    gp_layer_group_menu->ot = op->type;
    move_to_layer_group_menus_create(op, gp_layer_group_menu);

    uiPopupMenu *pup = UI_popup_menu_begin(C, IFACE_("Move to Group"), ICON_NONE);
    uiLayout *layout = UI_popup_menu_layout(pup);
    uiLayoutSetOperatorContext(layout, WM_OP_INVOKE_DEFAULT);
    move_to_layer_group_menu_draw(C, layout, gp_layer_group_menu);
    UI_popup_menu_end(C, pup);

    return OPERATOR_INTERFACE;
  }
  return grease_pencil_layer_group_manage_exec(C, op);
}

/* The get_description callback must match the expected signature (returning const char *) */
static std::string grease_pencil_layer_group_manage_get_description(bContext * /*C*/,
                                                                    wmOperatorType *ot,
                                                                    PointerRNA *ptr)
{
  int action = RNA_enum_get(ptr, "action");
  if (action == LAYER_GROUP_ACTION_MOVE_INTO) {
    return TIP_("Move selected layers into a group");
  }
  else if (action == LAYER_GROUP_ACTION_GROUP) {
    return TIP_("Group selected layers");
  }
  else if (action == LAYER_GROUP_ACTION_UNGROUP) {
    return TIP_("Ungroup selected layers");
  }
  return ot->description;
}

bool active_grease_pencil_layer_or_group_poll(bContext *C)
{
  const GreasePencil *grease_pencil = blender::ed::greasepencil::from_context(*C);
  bke::greasepencil::TreeNode *active_node = const_cast<bke::greasepencil::TreeNode *>(
      grease_pencil->get_active_node());

  if (active_node == nullptr) {
    CTX_wm_operator_poll_msg_set(C, "No active layer or group");
    return false;
  }

  /* If the active node is a group, allow poll even if it has only one child. */
  if (active_node->is_group()) {
    return true;
  }

  /* Otherwise, if active node belongs to a group, require at least 2 nodes. */
  const LayerGroup *parent = active_node->parent_group();
  if (parent != nullptr) {
    if (parent->num_direct_nodes() < 2) {
      CTX_wm_operator_poll_msg_set(C, "Group must have at least 2 layers");
      return false;
    }
  }
  return true;
}

static void GREASE_PENCIL_OT_layer_group_manage(wmOperatorType *ot)
{
  ot->name = "Manage Layer Group";
  ot->idname = "GREASE_PENCIL_OT_layer_group_manage";
  ot->description = "Group, ungroup or move selected layers into a group";
  ot->exec = grease_pencil_layer_group_manage_exec;
  ot->invoke = grease_pencil_layer_group_manage_invoke;
  ot->poll = active_grease_pencil_layer_or_group_poll;
  ot->get_description = grease_pencil_layer_group_manage_get_description;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "action",
               layer_group_action_items,
               LAYER_GROUP_ACTION_GROUP,
               "Action",
               "Action to perform on the selected layers");
  RNA_def_boolean(
      ot->srna, "delete_empty", true, "Delete Empty", "Delete group if empty after ungrouping");
  /* Properties for MOVE_INTO. */
  RNA_def_int(ot->srna,
              "target_group_index",
              0,
              0,
              INT_MAX,
              "Target Group Index",
              "Index of the target group in the list of groups",
              0,
              INT_MAX);
  RNA_def_boolean(ot->srna,
                  "is_new",
                  false,
                  "Create New",
                  "Create a new group instead of using an existing one");
  RNA_def_string(
      ot->srna, "new_group_name", nullptr, MAX_NAME, "New Group Name", "Name for the new group");
}

}  // namespace blender::ed::greasepencil

void ED_operatortypes_grease_pencil_layers()
{
  using namespace blender::ed::greasepencil;
  WM_operatortype_append(GREASE_PENCIL_OT_layer_add);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_remove);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_move);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_active);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_hide);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_reveal);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_isolate);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_lock_all);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_duplicate);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_merge);

  WM_operatortype_append(GREASE_PENCIL_OT_layer_group_add);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_group_remove);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_mask_add);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_mask_remove);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_mask_reorder);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_group_color_tag);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_duplicate_object);
  WM_operatortype_append(GREASE_PENCIL_OT_layer_group_manage);
}
