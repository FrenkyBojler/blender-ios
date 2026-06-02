/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <iostream>

#include "BLI_bounds.hh"
#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BKE_action.hh"
#include "BKE_appdir.hh"
#include "BKE_blender_copybuffer.hh"
#include "BKE_blendfile.hh"
#include "BKE_context.hh"
#include "BKE_fcurve.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_types.hh"

#include "ED_anim_transformable.hh"
#include "ED_screen.hh"

#include "ANIM_action.hh"

#include "anim_intern.hh"

namespace blender::ed::animrig {

constexpr const char *clipboard_name = "world_space_buffer.blend";

static void matrix_to_fcurves(const float4x4 &matrix,
                              Span<FCurve *> fcurves,
                              const int frame,
                              const int key_index)
{
  /* Using IndexRange(3) because the last row is always 0/0/0/1 which means we don't need to
   * store it. */
  for (const int y : IndexRange(3)) {
    for (const int x : IndexRange(4)) {
      FPoint &fpt = fcurves[y * 4 + x]->fpt[key_index];
      fpt.vec[0] = frame;
      fpt.vec[1] = matrix[x][y];
    }
  }
}

static float4x4 fcurves_to_matrix(const Span<const FCurve *> fcurves, const int key_index)
{
  float4x4 mat = float4x4::identity();
  for (const int y : IndexRange(3)) {
    for (const int x : IndexRange(4)) {
      FPoint &fpt = fcurves[y * 4 + x]->fpt[key_index];
      mat[x][y] = fpt.vec[1];
    }
  }
  return mat;
}

/* TODO move this to the AnimTransformable class. */
static float4x4 get_evaluated_world_space(const Depsgraph &dg,
                                          const AnimTransformable &transformable)
{
  ID *eval_id = DEG_get_evaluated_id(&dg, transformable.owner_id());
  BLI_assert(eval_id);
  switch (transformable.type()) {
    case AnimTransformable::Type::POSE_BONE: {
      Object *ob_eval = id_cast<Object *>(eval_id);
      bPoseChannel *pose_bone_eval = BKE_pose_channel_find_name(ob_eval->pose,
                                                                transformable.name().data());
      if (!pose_bone_eval) {
        BLI_assert_unreachable();
        break;
      }
      return ob_eval->object_to_world() * float4x4(pose_bone_eval->pose_mat);
    }
  }
  return float4x4::identity();
}

static float4x4 world_to_local(const Depsgraph &dg,
                               const AnimTransformable &transformable,
                               const float4x4 &world_matrix)
{
  ID *eval_id = DEG_get_evaluated_id(&dg, transformable.owner_id());
  BLI_assert(eval_id);
  switch (transformable.type()) {
    case AnimTransformable::Type::POSE_BONE: {
      Object *ob_eval = id_cast<Object *>(eval_id);
      bPoseChannel *pose_bone_eval = BKE_pose_channel_find_name(ob_eval->pose,
                                                                transformable.name().data());
      if (!pose_bone_eval) {
        BLI_assert_unreachable();
        break;
      }
      float4x4 pose_mat_inv = math::invert(float4x4(pose_bone_eval->pose_mat));
      return ob_eval->world_to_object() * pose_mat_inv * world_matrix;
    }
  }
  return float4x4::identity();
}

static Vector<ID *> get_unique_ids(const Span<AnimTransformable> transformables)
{
  /* We need the ID pointers to build the depsgraph, but every ID in the Vector
   * should be unique. */
  Vector<ID *> ids;
  Set<ID *> added_ids;
  for (const AnimTransformable &transformable : transformables) {
    if (added_ids.add(transformable.owner_id())) {
      ids.append(transformable.owner_id());
    }
  }
  return ids;
}

struct GraphNode {
  AnimTransformable *transformable = nullptr;
  bool inserted = false;
  /* Other nodes that need to be applied before this. */
  Vector<GraphNode *> ancestors;

  bool can_insert()
  {
    if (inserted) {
      /* Already applied. Don't apply twice. */
      return false;
    }
    for (GraphNode *node : ancestors) {
      if (!node->inserted) {
        /* All ancestors must be applied before this node. */
        return false;
      }
    }
    return true;
  }
};

static Array<GraphNode> build_transformable_ancestry(
    const Depsgraph *depsgraph, const MutableSpan<AnimTransformable> transformables)
{
  using DegComponentIdentifier = std::pair<ID *, StringRef>;

  Array<GraphNode> nodes(transformables.size());
  Map<DegComponentIdentifier, GraphNode *> component_map;
  Set<DegComponentIdentifier> visited_components;

  for (const int i : transformables.index_range()) {
    AnimTransformable &t = transformables[i];
    nodes[i].transformable = &t;
    /* TODO handle objects which wouldn't have a component name. */
    component_map.add({t.owner_id(), t.name()}, &nodes[i]);
  }

  for (GraphNode &graph_node : nodes) {
    AnimTransformable *transformable = graph_node.transformable;

    DEG_foreach_dependent_component(
        depsgraph,
        transformable->owner_id(),
        DEG_OB_COMP_BONE,
        transformable->name(),
        [&](ID *other_id, eDepsObjectComponentType component, StringRef component_name) {
          if (!ELEM(component, DEG_OB_COMP_TRANSFORM, DEG_OB_COMP_BONE)) {
            return true;
          }
          if (component_name == transformable->name()) {
            /* Skip self. */
            return true;
          }
          DegComponentIdentifier cid(other_id, component_name);
          if (visited_components.contains(cid)) {
            return false;
          }
          GraphNode *dependent_node = component_map.lookup_default(cid, nullptr);
          if (!dependent_node) {
            return true;
          }
          dependent_node->ancestors.append(&graph_node);
          visited_components.add(cid);
          return true;
        });
  }
  return nodes;
}

static Vector<AnimTransformable *> depsgraph_sorted_transformables(
    const Depsgraph *depsgraph, const MutableSpan<AnimTransformable> transformables)
{
  Array<GraphNode> nodes = build_transformable_ancestry(depsgraph, transformables);
  Vector<AnimTransformable *> sorted_transformables;

  while (true) {
    bool inserted_any = false;
    for (GraphNode &graph_node : nodes) {
      if (!graph_node.can_insert()) {
        continue;
      }
      std::cout << "insert " << graph_node.transformable->name() << std::endl;
      sorted_transformables.append(graph_node.transformable);
      inserted_any = true;
      graph_node.inserted = true;
    }
    if (!inserted_any) {
      /* There are 2 cases in which this can happen. Either we applied all transforms, or there
       * is a dependency cycle where 2 nodes have each other in their ancestors. In the latter case
       * the returned Vector will not contain those transformables with a cycle.*/
      break;
    }
  }

  return sorted_transformables;
}

/* -------------------------------------------------------------------- */
/** \name Main Functions
 * \{ */

/**
 * \param range inclusive/exclusive
 */
static void copy_world_space(Main &bmain,
                             Scene &scene,
                             ViewLayer &view_layer,
                             ReportList &reports,
                             const Span<AnimTransformable> transformables,
                             const Bounds<int> range)
{
  namespace ar = blender::animrig;
  namespace bf = bke::blendfile;
  bf::PartialWriteContext copybuffer{bmain};
  bAction *dna_action = reinterpret_cast<bAction *>(
      copybuffer.id_create(ID_AC,
                           "world_space_copy",
                           nullptr,
                           {(bf::PartialWriteContext::IDAddOperations::SET_FAKE_USER |
                             bf::PartialWriteContext::IDAddOperations::SET_CLIPBOARD_MARK)}));

  /* Using the frame start and end of the action to store the range of the copied keys. */
  dna_action->frame_start = range.min;
  dna_action->frame_end = range.max;
  ar::Action &action = dna_action->wrap();
  action.layer_keystrip_ensure();
  ar::StripKeyframeData &strip_data = action.layer(0)->strip(0)->data<ar::StripKeyframeData>(
      action);
  ar::Slot &slot = action.slot_add();
  ar::Channelbag &channelbag = strip_data.channelbag_for_slot_ensure(slot);

  /* We are storing the world space matrix in separate FCurves so the data can be stored in a
   * blend file. */
  Array<Array<FCurve *>> world_space_data(transformables.size());
  for (const int transformable_index : transformables.index_range()) {
    const AnimTransformable &transformable = transformables[transformable_index];
    Array<FCurve *> fcurves(12);
    for (const int i : fcurves.index_range()) {
      FCurve *fcurve = BKE_fcurve_create();
      fcurve->rna_path = BLI_strdupn(transformable.name().data(), transformable.name().size());
      fcurve->array_index = i;
      /* Using FPoint because we only need 2 floats per key, not the huge struct that
       * is BezTriple.  */
      fcurve->fpt = MEM_new_array_uninitialized<FPoint>(range.size(), "world_space_copy_points");
      fcurve->totvert = range.size();
      /* Could allocate space on the channelbag in big chunks instead of appending which is a
       * MEM_new every time. */
      channelbag.fcurve_append(*fcurve);
      fcurves[i] = fcurve;
    }

    world_space_data[transformable_index] = std::move(fcurves);
  }

  Depsgraph *depsgraph = DEG_graph_new(&bmain, &scene, &view_layer, DAG_EVAL_VIEWPORT);
  Vector<ID *> ids = get_unique_ids(transformables);
  DEG_graph_build_from_ids(depsgraph, ids);

  for (int frame = range.min; frame < range.max; frame++) {
    const int key_index = frame - range.min;
    DEG_evaluate_on_framechange(depsgraph, frame);
    for (const int transformable_index : transformables.index_range()) {
      const AnimTransformable &transformable = transformables[transformable_index];
      const float4x4 world_matrix = get_evaluated_world_space(*depsgraph, transformable);
      matrix_to_fcurves(world_matrix, world_space_data[transformable_index], frame, key_index);
    }
  }

  DEG_graph_free(depsgraph);

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_base(), clipboard_name);
  BLI_assert(copybuffer.is_valid());
  copybuffer.write_as_copypaste_buffer(filepath, reports);
}

static void paste_world_space(Main &bmain,
                              Scene &scene,
                              ViewLayer &view_layer,
                              ReportList &reports,
                              const MutableSpan<AnimTransformable> transformables)
{
  namespace ar = blender::animrig;

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_base(), clipboard_name);
  Main *clipboard_bmain = BKE_main_new();

  if (!BKE_copybuffer_read(clipboard_bmain, filepath, &reports, FILTER_ID_AC)) {
    BKE_report(&reports, RPT_ERROR, "No clipboard to read from");
    BKE_main_free(clipboard_bmain);
    return;
  }

  if (clipboard_bmain->actions.is_empty()) {
    BKE_report(&reports, RPT_ERROR, "Clipboard data has no animation");
    BKE_main_free(clipboard_bmain);
    return;
  }

  bAction *dna_action = reinterpret_cast<bAction *>(clipboard_bmain->actions.first);
  ar::Action &action = dna_action->wrap();
  if (action.strip_keyframe_data().is_empty() ||
      action.strip_keyframe_data()[0]->channelbags().is_empty())
  {
    BKE_report(&reports, RPT_ERROR, "Clipboard data has no animation");
    BKE_main_free(clipboard_bmain);
    return;
  }

  ar::Channelbag &channelbag = *action.strip_keyframe_data()[0]->channelbags()[0];
  Map<StringRefNull, Array<FCurve *>> world_space_data;
  for (FCurve *fcurve : channelbag.fcurves()) {
    BLI_assert(fcurve != nullptr);
    Array<FCurve *> &fcurves = world_space_data.lookup_or_add(fcurve->rna_path,
                                                              Array<FCurve *>(12));
    fcurves[fcurve->array_index] = fcurve;
  }

  for (Array<FCurve *> &fcurves : world_space_data.values()) {
    for (FCurve *fcurve : fcurves) {
      if (fcurve == nullptr) {
        BKE_report(&reports, RPT_ERROR, "Clipboard contains incomplete animation data");
        return;
      }
    }
  }

  /* Build a minimal depsgraph because we need to evaluate the scene on every frame to correctly
   * invert world to local space. */
  Depsgraph *depsgraph = DEG_graph_new(&bmain, &scene, &view_layer, DAG_EVAL_VIEWPORT);
  Vector<ID *> ids = get_unique_ids(transformables);
  DEG_graph_build_from_ids(depsgraph, ids);

  /* We need to first apply the transformation to those entities that are not affected by any other
   * transformables. This is why we need to sort using the depsgraph. */
  Vector<AnimTransformable *> sorted_transformables = depsgraph_sorted_transformables(
      depsgraph, transformables);

  if (sorted_transformables.size() != transformables.size()) {
    BKE_report(&reports, RPT_ERROR, "Failed to paste all transforms. Potential dependency cycle");
  }

  /* We need to write local transform values to an intermediate buffer because inserting keys while
   * iterating would change the interpolation to the next frame and thus the result. */
  const Bounds<int> range = {int(dna_action->frame_start), int(dna_action->frame_end)};
  Array<Array<float4x4>> local_matrix_buffer(sorted_transformables.size());
  for (const int i : sorted_transformables.index_range()) {
    local_matrix_buffer[i].reinitialize(range.size());
  }
  for (int frame = range.min; frame < range.max; frame++) {
    DEG_evaluate_on_framechange(depsgraph, frame);
    /* Assuming that all FCurves have the same vertex count and their keys on the same frames. */
    const int key_index = frame - range.min;
    for (const int i : sorted_transformables.index_range()) {
      AnimTransformable *transformable = sorted_transformables[i];
      const Array<FCurve *> *fcurves = world_space_data.lookup_ptr(transformable->name());
      if (!fcurves) {
        continue;
      }
      const float4x4 world_matrix = fcurves_to_matrix(*fcurves, key_index);
      const float4x4 local_matrix = world_to_local(*depsgraph, *transformable, world_matrix);
      /* It is still important to apply the matrix here because otherwise the world to local
       * calculations of dependent transformables won't be correct. */
      transformable->set_local_matrix(local_matrix);
      local_matrix_buffer[i][key_index] = std::move(local_matrix);
    }
  }

  DEG_graph_free(depsgraph);
  BKE_main_free(clipboard_bmain);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operators
 * \{ */

/* TODO: copied from rotation conversion PR. Refactor once that lands. */
static Vector<AnimTransformable> selected_transformables_from_context(bContext *C)
{
  Vector<AnimTransformable> transformables;
  Vector<PointerRNA> pointers;
  switch (CTX_data_mode_enum(C)) {

    case CTX_MODE_POSE: {
      CTX_data_selected_pose_bones(C, &pointers);
      for (PointerRNA &ptr : pointers) {
        transformables.append(
            {*id_cast<Object *>(ptr.owner_id), *static_cast<bPoseChannel *>(ptr.data)});
      }
      break;
    }

    default:
      break;
  }
  return transformables;
}

static wmOperatorStatus world_space_copy_exec(bContext *C, wmOperator *op)
{
  Vector<AnimTransformable> transformables = selected_transformables_from_context(C);

  Bounds<int> bounds = {RNA_int_get(op->ptr, "start"), RNA_int_get(op->ptr, "end")};
  if (bounds.is_empty()) {
    BKE_reportf(op->reports, RPT_ERROR, "Invalid frame range %d-%d", bounds.min, bounds.max);
    return OPERATOR_CANCELLED;
  }
  copy_world_space(*CTX_data_main(C),
                   *CTX_data_scene(C),
                   *CTX_data_view_layer(C),
                   *op->reports,
                   transformables,
                   bounds);
  return OPERATOR_FINISHED;
}

static bool world_space_copy_poll(bContext *C)
{
  return ED_operator_posemode(C);
}

void ANIM_OT_world_space_copy(wmOperatorType *ot)
{
  ot->name = "Copy World Space";
  ot->idname = "ANIM_OT_world_space_copy";
  ot->description = "Copy animation from selected elements to the clipboard";

  ot->exec = world_space_copy_exec;
  ot->poll = world_space_copy_poll;

  /* No undo possible since this creates data outside the current blend file. */
  ot->flag = OPTYPE_REGISTER;

  RNA_def_int(
      ot->srna, "start", 0, -INT_MAX, INT_MAX, "Start", "Start frame to copy from", 0, INT_MAX);
  RNA_def_int(
      ot->srna, "end", 250, -INT_MAX, INT_MAX, "End", "End frame to copy from", 0, INT_MAX);
}

static wmOperatorStatus world_space_paste_exec(bContext *C, wmOperator *op)
{
  Vector<AnimTransformable> transformables = selected_transformables_from_context(C);
  paste_world_space(*CTX_data_main(C),
                    *CTX_data_scene(C),
                    *CTX_data_view_layer(C),
                    *op->reports,
                    transformables);
  return OPERATOR_FINISHED;
}

static bool world_space_paste_poll(bContext *C)
{
  return ED_operator_posemode(C);
}

void ANIM_OT_world_space_paste(wmOperatorType *ot)
{
  ot->name = "Paste World Space";
  ot->idname = "ANIM_OT_world_space_paste";
  ot->description = "Paste the animation from the clipboard to selected elements";

  ot->exec = world_space_paste_exec;
  ot->poll = world_space_paste_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

}  // namespace blender::ed::animrig
