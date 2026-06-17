/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <iostream>

#include "BLI_bounds.hh"
#include "BLI_listbase.hh"
#include "BLI_math_rotation_c.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"

#include "BKE_action.hh"
#include "BKE_appdir.hh"
#include "BKE_armature.hh"
#include "BKE_blender_copybuffer.hh"
#include "BKE_blendfile.hh"
#include "BKE_context.hh"
#include "BKE_fcurve.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_anim_transformable.hh"
#include "ED_screen.hh"

#include "ANIM_action.hh"
#include "ANIM_animdata.hh"
#include "ANIM_rna.hh"

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

static Vector<ID *> get_unique_ids(const Span<AnimTransformable *> transformables)
{
  /* We need the ID pointers to build the depsgraph, but every ID in the Vector
   * should be unique. */
  Vector<ID *> ids;
  Set<ID *> added_ids;
  for (const AnimTransformable *transformable : transformables) {
    if (added_ids.add(transformable->owner_id())) {
      ids.append(transformable->owner_id());
    }
  }
  return ids;
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

/* Stores which other AnimTransformables have to be applied before it. */
struct TransformableRelations {
  AnimTransformable *transformable = nullptr;
  /* Other transformable_relations that need to be applied before this. */
  Vector<TransformableRelations *> ancestors = {};
  /* Indicates that this data has been processed. */
  bool done = false;

  bool can_insert()
  {
    if (done) {
      /* Already applied. Don't apply twice. */
      return false;
    }
    for (TransformableRelations *ancestor : ancestors) {
      if (!ancestor->done) {
        /* All ancestors must be applied before this transformable. */
        return false;
      }
    }
    return true;
  }
};

/* Uniquely Identifies a component of the depsgraph. */
struct DegComponentIdentifier {
  ID *id = nullptr;
  StringRef name;
  eDepsObjectComponentType type;

  bool operator==(const DegComponentIdentifier &other) const
  {
    return id == other.id && type == other.type && name == other.name;
  }

  uint64_t hash() const
  {
    return get_default_hash(id, type, name);
  }
};

static DegComponentIdentifier transformable_to_deg_identifier(
    const AnimTransformable &transformable)
{
  switch (transformable.type()) {
    case AnimTransformable::Type::POSE_BONE:
      return {transformable.owner_id(), transformable.name(), DEG_OB_COMP_BONE};

    case AnimTransformable::Type::OBJECT:
      return {transformable.owner_id(), "", DEG_OB_COMP_TRANSFORM};
  }

  BLI_assert_unreachable();
  return {transformable.owner_id(), "", DEG_OB_COMP_TRANSFORM};
}

static Array<TransformableRelations> build_transformable_relations(
    const Depsgraph *depsgraph, const MutableSpan<AnimTransformable *> transformables)
{
  Array<TransformableRelations> transformable_relations(transformables.size());
  Map<DegComponentIdentifier, TransformableRelations *> component_map;

  for (const int i : transformables.index_range()) {
    AnimTransformable &transformable = *transformables[i];
    transformable_relations[i].transformable = &transformable;
    transformable_relations[i].done = false;
    component_map.add(transformable_to_deg_identifier(transformable), &transformable_relations[i]);
  }

  for (const DegComponentIdentifier &deg_identifier : component_map.keys()) {
    TransformableRelations *transformable_relation = component_map.lookup(deg_identifier);
    DEG_foreach_dependent_component(
        depsgraph,
        deg_identifier.id,
        deg_identifier.type,
        deg_identifier.name,
        [&](ID *other_id, eDepsObjectComponentType type, StringRef component_name) {
          if (!ELEM(type, DEG_OB_COMP_TRANSFORM, DEG_OB_COMP_BONE)) {
            return true;
          }
          DegComponentIdentifier dependent_deg_id(other_id, component_name, type);
          if (dependent_deg_id == deg_identifier) {
            /* Skip self. */
            return true;
          }
          TransformableRelations *dependent = component_map.lookup_default(dependent_deg_id,
                                                                           nullptr);
          if (!dependent) {
            return true;
          }
          dependent->ancestors.append(transformable_relation);
          if (dependent->done) {
            /* This is used as an optimization. If we already visited that component for a previous
             * transformable, the dependency of that branch is already recorded and we can stop
             * iterating into it. */
            return false;
          }
          dependent->done = true;
          return true;
        });
  }
  for (TransformableRelations &relation : transformable_relations) {
    /* Resetting the bool for the next use. */
    relation.done = false;
  }
  return transformable_relations;
}

static Vector<AnimTransformable *> depsgraph_sorted_transformables(
    const Depsgraph *depsgraph, const MutableSpan<AnimTransformable *> transformables)
{
  Array<TransformableRelations> transformable_relations = build_transformable_relations(
      depsgraph, transformables);
  Vector<AnimTransformable *> sorted_transformables;

  while (true) {
    bool inserted_any = false;
    for (TransformableRelations &relations : transformable_relations) {
      if (!relations.can_insert()) {
        continue;
      }
      sorted_transformables.append(relations.transformable);
      inserted_any = true;
      relations.done = true;
    }
    if (!inserted_any) {
      /* There are 2 cases in which this can happen. Either we applied all transforms, or there
       * is a dependency cycle where 2 transformable_relations have each other in their
       * ancestors. In the latter case the returned Vector will not contain those transformables
       * with a cycle.*/
      break;
    }
  }

  return sorted_transformables;
}

struct PasteFCurve {
  FCurve *fcurve = nullptr;
  /* Store info if that FCurve was created by this code. If yes, we can potentially remove it if
   * the keys are all on the same value after pasting. */
  bool created_on_paste = false;
  /* The index into the bezt array from which to start pasting. */
  int paste_start_index = 0;
};

struct TransformFCurves {
  Array<PasteFCurve, 3> loc;
  eRotationModes rotation_mode;
  Array<PasteFCurve, 4> rot;
  Array<PasteFCurve, 3> scale;

  TransformFCurves()
  {
    loc.reinitialize(3);
    rot.reinitialize(4);
    scale.reinitialize(3);
  }
};

static void ensure_baked_fcurves(Main &bmain,
                                 MutableSpan<PasteFCurve> fcus,
                                 blender::animrig::Channelbag &channelbag,
                                 const StringRefNull rna_path,
                                 const Bounds<int> range)
{
  namespace ar = blender::animrig;

  bool has_key_on_frame = false;
  /* Ensuring all FCurves exist. */
  for (const int i : fcus.index_range()) {
    PasteFCurve &paste_fcu = fcus[i];
    if (!paste_fcu.fcurve) {
      /* TODO pass group name. */
      FCurve &fcurve = channelbag.fcurve_ensure(&bmain, {rna_path, i, PROP_FLOAT, PROP_NONE});
      paste_fcu.fcurve = &fcurve;
      paste_fcu.created_on_paste = true;
    }
    ar::bake_fcurve(paste_fcu.fcurve, {range.min, range.max}, 1, ar::BakeCurveRemove::IN_RANGE);
    paste_fcu.paste_start_index = BKE_fcurve_bezt_binarysearch_index(
        paste_fcu.fcurve->bezt, range.min, paste_fcu.fcurve->totvert, &has_key_on_frame);
    BLI_assert(has_key_on_frame);
  }
}

static void set_keys_to_transform(TransformFCurves &t_fcus,
                                  const float4x4 &matrix,
                                  const int paste_index)
{
  const float3 location = matrix.location();
  const float3 scale = math::to_scale(matrix);
  Rotation rotation_quat;
  rotation_quat.mode = ROT_MODE_QUAT;
  const float4 quat = float4(math::to_quaternion(matrix));
  rotation_quat.values.reinitialize(4);
  copy_qt_qt(rotation_quat.values.data(), quat);
  /* TODO pass reference rotation to avoid gimbal lock. */
  Rotation rotation = rotation_quat.converted_to_mode(t_fcus.rotation_mode);

  for (const int i : t_fcus.loc.index_range()) {
    PasteFCurve &pfcu = t_fcus.loc[i];
    BLI_assert(pfcu.paste_start_index + paste_index < pfcu.fcurve->totvert);
    const int bezt_index = pfcu.paste_start_index + paste_index;
    BezTriple &key = pfcu.fcurve->bezt[bezt_index];
    BKE_fcurve_keyframe_move_value_with_handles(&key, location[i]);
  }

  for (const int i : t_fcus.rot.index_range()) {
    PasteFCurve &pfcu = t_fcus.rot[i];
    BLI_assert(pfcu.paste_start_index + paste_index < pfcu.fcurve->totvert);
    const int bezt_index = pfcu.paste_start_index + paste_index;
    BezTriple &key = pfcu.fcurve->bezt[bezt_index];
    BKE_fcurve_keyframe_move_value_with_handles(&key, rotation.values[i]);
  }

  for (const int i : t_fcus.scale.index_range()) {
    PasteFCurve &pfcu = t_fcus.scale[i];
    BLI_assert(pfcu.paste_start_index + paste_index < pfcu.fcurve->totvert);
    const int bezt_index = pfcu.paste_start_index + paste_index;
    BezTriple &key = pfcu.fcurve->bezt[bezt_index];
    BKE_fcurve_keyframe_move_value_with_handles(&key, scale[i]);
  }
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
      /* TODO this is not sufficient to identify a bone since they can have identical names in
       * different armatures. */
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
      const float4x4 world_matrix = get_world_space(*depsgraph, transformable);
      matrix_to_fcurves(world_matrix, world_space_data[transformable_index], frame, key_index);
    }
  }

  DEG_graph_free(depsgraph);

  char filepath[FILE_MAX];
  BLI_path_join(filepath, sizeof(filepath), BKE_tempdir_base(), clipboard_name);
  BLI_assert(copybuffer.is_valid());
  copybuffer.write_as_copypaste_buffer(filepath, reports);
}

static Vector<AnimTransformable *> pasteable_transformables(
    const MutableSpan<AnimTransformable> transformables,
    const Map<StringRefNull, Array<FCurve *>> &world_space_data)
{
  Vector<AnimTransformable *> pasteables;
  /* TODO: more sophisticated logic matching data in the world space buffer with transformables to
   * paste on. */
  for (AnimTransformable &transformable : transformables) {
    const Array<FCurve *> *fcurves = world_space_data.lookup_ptr(transformable.name());
    if (fcurves) {
      pasteables.append(&transformable);
    }
  }
  return pasteables;
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
        BKE_main_free(clipboard_bmain);
        return;
      }
    }
  }

  Vector<AnimTransformable *> pasteables = pasteable_transformables(transformables,
                                                                    world_space_data);
  if (pasteables.size() == 0) {
    BKE_report(&reports, RPT_ERROR, "Cannot match selection to clipboard data");
    BKE_main_free(clipboard_bmain);
    return;
  }

  /* Build a minimal depsgraph because we need to evaluate the scene on every frame to correctly
   * invert world to local space. */
  Depsgraph *depsgraph = DEG_graph_new(&bmain, &scene, &view_layer, DAG_EVAL_VIEWPORT);
  Vector<ID *> ids = get_unique_ids(pasteables);
  DEG_graph_build_from_ids(depsgraph, ids);

  /* We need to first apply the transformation to those entities that are not affected by any other
   * transformables. This is why we need to sort using the depsgraph. */
  Vector<AnimTransformable *> sorted_transformables = depsgraph_sorted_transformables(depsgraph,
                                                                                      pasteables);

  if (sorted_transformables.size() != pasteables.size()) {
    BKE_report(&reports, RPT_ERROR, "Failed to paste all transforms. Potential dependency cycle");
  }

  const Bounds<int> range = {int(dna_action->frame_start), int(dna_action->frame_end)};
  /* We have to ensure every frame of the affected range has a key. Otherwise inserting keys will
   * modify the interpolation of the following frames. */
  Array<TransformFCurves> fcurve_buffer(sorted_transformables.size());
  for (const int i : sorted_transformables.index_range()) {
    AnimTransformable *transformable = sorted_transformables[i];
    ID *owner_id = transformable->owner_id();
    bAction *dna_action = ar::id_action_ensure(&bmain, owner_id);
    /* When adding layers this becomes a lot more complicated. We'll have to answer where keys go
     * in this case. Multiple things to consider:
     * - The active layer may have no effect on the final pose
     * - Not every layer may have a channelbag for this transformable.
     * - When inserting keys into a layer that is additive, we need to adjust the inserted values.
     * - When inserting keys into a layer with an influence < 1 we'll also have to adjust the
     * values.
     * - In all cases, the result has to be that the final world space of the transformable ends up
     * where it was copied from.
     */
    ar::assert_baklava_phase_1_invariants(action);
    ar::Channelbag &channelbag = ar::action_channelbag_ensure(*dna_action, *owner_id);
    TransformFCurves &fcus = fcurve_buffer[i];
    fcus.rotation_mode = transformable->get_rotation_mode();
    if (fcus.rotation_mode >= ROT_MODE_EUL) {
      fcus.rot.reinitialize(3);
    }
    else {
      fcus.rot.reinitialize(4);
    }
    const std::string loc_path = transformable->rna_path_to_property(
        AnimTransformable::PropertyType::LOCATION);
    /* This will fail if the rotation mode is animated to jump from e.g. euler to quaternion. */
    const std::string rot_path = transformable->rna_path_to_property(
        AnimTransformable::PropertyType::ROTATION);
    const std::string scale_path = transformable->rna_path_to_property(
        AnimTransformable::PropertyType::SCALE);

    for (FCurve *fcurve : channelbag.fcurves()) {
      StringRefNull fcurve_path(fcurve->rna_path);
      if (fcurve_path == loc_path) {
        fcus.loc[fcurve->array_index].fcurve = fcurve;
      }
      else if (fcurve_path == rot_path) {
        fcus.rot[fcurve->array_index].fcurve = fcurve;
      }
      else if (fcurve_path == scale_path) {
        fcus.scale[fcurve->array_index].fcurve = fcurve;
      }
    }

    /* Ensuring all FCurves exist. */
    ensure_baked_fcurves(bmain, fcus.loc, channelbag, loc_path, range);
    ensure_baked_fcurves(bmain, fcus.rot, channelbag, rot_path, range);
    ensure_baked_fcurves(bmain, fcus.scale, channelbag, scale_path, range);
  }

  /* Since we potentially added FCurves, we have to rebuild the depsgraph. */
  DEG_graph_build_from_ids(depsgraph, ids);

  for (int frame = range.min; frame < range.max; frame++) {
    /* Assuming that all FCurves have the same vertex count and their keys on the same frames. */
    const int key_index = frame - range.min;
    for (const int i : sorted_transformables.index_range()) {
      AnimTransformable *transformable = sorted_transformables[i];
      const Array<FCurve *> *fcurves = world_space_data.lookup_ptr(transformable->name());
      BLI_assert_msg(fcurves != nullptr,
                     "Only transformables with matching matrix data should iterated here");
      const float4x4 world_matrix = fcurves_to_matrix(*fcurves, key_index);
      /* We need the depsgraph evaluation in the inner loop so the position of dependents is
       * updated. This is potentially very slow. */
      DEG_evaluate_on_framechange(depsgraph, frame);
      const float4x4 local_matrix = world_to_local(*depsgraph, *transformable, world_matrix);
      TransformFCurves &t_fcus = fcurve_buffer[i];
      set_keys_to_transform(t_fcus, local_matrix, key_index);
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
    case CTX_MODE_OBJECT: {
      CTX_data_selected_objects(C, &pointers);
      for (PointerRNA &ptr : pointers) {
        transformables.append(ed::AnimTransformable(*id_cast<Object *>(ptr.owner_id)));
      }
      break;
    }

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
  return ED_operator_posemode(C) || ED_operator_objectmode(C);
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
  for (AnimTransformable &t : transformables) {
    DEG_id_tag_update(t.owner_id(), ID_RECALC_ANIMATION);
    WM_event_add_notifier(C, NC_OBJECT | ND_POSE, t.owner_id());
  }

  return OPERATOR_FINISHED;
}

static bool world_space_paste_poll(bContext *C)
{
  return ED_operator_posemode(C) || ED_operator_objectmode(C);
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
