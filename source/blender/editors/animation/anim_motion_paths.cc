/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "MEM_guardedalloc.h"

#include <cstdlib>

#include "BLI_bounds.hh"
#include "BLI_listbase.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_c.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_string.hh"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_scene_types.h"

#include "BKE_action.hh"
#include "BKE_anim_data.hh"
#include "BKE_camera.h"
#include "BKE_fcurve.hh"
#include "BKE_main.hh"
#include "BKE_scene.hh"
#include "BKE_wm_runtime.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "GPU_batch.hh"
#include "GPU_vertex_buffer.hh"

#include "ED_anim_api.hh"
#include "ED_keyframes_keylist.hh"

#include "WM_api.hh"

#include "ANIM_action.hh"
#include "ANIM_action_legacy.hh"
#include "ANIM_animdata.hh"
#include "ANIM_bone_collections.hh"

#include "CLG_log.h"

namespace blender {

static CLG_LogRef LOG = {"anim.motion_paths"};

/* ........ */

Depsgraph *animviz_depsgraph_build(Main *bmain,
                                   Scene *scene,
                                   ViewLayer *view_layer,
                                   const Span<MPathTarget> targets)
{
  /* Allocate dependency graph. */
  Depsgraph *depsgraph = DEG_graph_new(bmain, scene, view_layer, DAG_EVAL_VIEWPORT);

  /* Make a flat array of IDs for the DEG API. */
  Array<ID *> ids(targets.size());
  int current_id_index = 0;
  for (const MPathTarget &mpt : targets) {
    ids[current_id_index++] = &mpt.ob->id;
  }

  /* Build graph from all requested IDs. */
  DEG_graph_build_from_ids(depsgraph, ids);

  return depsgraph;
}

void animviz_build_motionpath_targets(Object *ob, Vector<MPathTarget> &r_targets)
{
  /* TODO: it would be nice in future to be able to update objects dependent on these bones too? */
  /* Object itself first. */
  if (ob->mpath) {
    /* New target for object. */
    MPathTarget mpt;
    mpt.mpath = ob->mpath;
    mpt.ob = ob;

    r_targets.append(mpt);
  }

  /* Bones. */
  if (ob->pose) {
    bArmature *arm = id_cast<bArmature *>(ob->data);
    for (bPoseChannel &pchan : ob->pose->chanbase) {
      if (!pchan.mpath) {
        continue;
      }
      Bone *bone = pchan.bone_get(*ob);
      if (!bone || !ANIM_bone_in_visible_collection(arm, bone)) {
        continue;
      }
      /* New target for bone. */
      MPathTarget mpt;
      mpt.mpath = pchan.mpath;
      mpt.ob = ob;
      mpt.pchan = &pchan;
      r_targets.append(mpt);
    }
  }
}

/* ........ */

/* Converts the given point into NDC space. */
static void transform_mpath_point_to_camera(Depsgraph &depsgraph,
                                            Object &camera,
                                            bMotionPathVert &mpv)
{
  Object *cam_eval = DEG_get_evaluated(&depsgraph, &camera);
  /* Aka projection matrix. */
  float4x4 window_matrix;
  Scene *scene = DEG_get_input_scene(&depsgraph);
  BKE_camera_multiview_window_matrix(&scene->r, cam_eval, nullptr, window_matrix.ptr());
  /* World to Object is the view matrix. */
  float4x4 perspective_matrix = window_matrix * cam_eval->world_to_object();
  const float4 co_clip_space = perspective_matrix * float4(mpv.co[0], mpv.co[1], mpv.co[2], 1.0);
  /* Storing the verts in NDC space which contains lens effects like sensor offset. See
   * `overlay_motion_path.hh/motion_path_sync`. Negative w values are behind the camera, thus
   * can't be correctly projected into the scene. Using abs(w) is consistent with
   * `project_point` in shader code. */
  const float3 co_ndc_space = float3(co_clip_space) /
                              math::max(math::abs(co_clip_space.w), 0.0001f);
  copy_v3_v3(mpv.co, co_ndc_space);
}

/* Perform baking for the targets on the current frame. */
static void motionpaths_calc_bake_target(const MPathTarget &mpt,
                                         const int cframe,
                                         Depsgraph *depsgraph,
                                         Object *camera)
{
  /* For each target, check if it can be baked on the current frame. */
  bMotionPath *mpath = mpt.mpath;

  /* Current frame must be within the range the cache works for.
   * - is inclusive of the first frame, but not the last otherwise we get buffer overruns.
   */
  if ((cframe < mpath->start_frame) || (cframe >= mpath->end_frame)) {
    return;
  }

  /* Get the relevant cache vert to write to. */
  bMotionPathVert &mpv = mpath->points[cframe - mpath->start_frame];

  Object *ob_eval = DEG_get_evaluated(depsgraph, mpt.ob);

  /* Lookup evaluated pose channel, here because the depsgraph
   * evaluation can change them so they are not cached in mpt. */
  bPoseChannel *pchan_eval = nullptr;
  if (mpt.pchan) {
    pchan_eval = BKE_pose_channel_find_name(ob_eval->pose, mpt.pchan->name);
  }

  /* Pose-channel or object path baking? */
  if (pchan_eval) {
    /* Heads or tails. */
    if (mpath->flag & MOTIONPATH_FLAG_BHEAD) {
      copy_v3_v3(mpv.co, pchan_eval->pose_head);
    }
    else {
      copy_v3_v3(mpv.co, pchan_eval->pose_tail);
    }

    /* Result must be in world-space. */
    mul_m4_v3(ob_eval->object_to_world().ptr(), mpv.co);
  }
  else {
    /* World-space object location. */
    copy_v3_v3(mpv.co, ob_eval->object_to_world().location());
  }

  if (mpath->flag & MOTIONPATH_FLAG_BAKE_CAMERA && camera) {
    transform_mpath_point_to_camera(*depsgraph, *camera, mpv);
  }

  /* Tag if it's a keyframe. */
  if (ED_keylist_find_exact(mpt.keylist, cframe)) {
    mpv.flag |= MOTIONPATH_VERT_KEY;
  }
  else {
    mpv.flag &= ~MOTIONPATH_VERT_KEY;
  }
  mpv->flag |= MOTIONPATH_VERT_EVALUATED;

  /* Incremental update on evaluated object if possible, for fast updating
   * while dragging in transform. */
  bMotionPath *mpath_eval = nullptr;
  if (mpt.pchan) {
    mpath_eval = (pchan_eval) ? pchan_eval->mpath : nullptr;
  }
  else {
    mpath_eval = ob_eval->mpath;
  }

  if (mpath_eval && mpath_eval->length == mpath->length) {
    bMotionPathVert &mpv_eval = mpath_eval->points[cframe - mpath_eval->start_frame];
    mpv_eval = mpv;

    GPU_VERTBUF_DISCARD_SAFE(mpath_eval->points_vbo);
    GPU_BATCH_DISCARD_SAFE(mpath_eval->batch_line);
    GPU_BATCH_DISCARD_SAFE(mpath_eval->batch_points);
  }
}

/* Get pointer to animviz settings for the given target. */
static bAnimVizSettings *animviz_target_settings_get(const MPathTarget &mpt)
{
  if (mpt.pchan != nullptr) {
    return &mpt.ob->pose->avs;
  }
  return &mpt.ob->avs;
}

void animviz_motionpath_compute_range(Object *ob, Scene *scene)
{
  bAnimVizSettings *avs = ob->mode == OB_MODE_POSE ? &ob->pose->avs : &ob->avs;

  if (avs->path_range == MOTIONPATH_RANGE_MANUAL) {
    /* Don't touch manually-determined ranges. */
    return;
  }

  const bool has_action = ob->adt && ob->adt->action;
  if (avs->path_range == MOTIONPATH_RANGE_SCENE || !has_action ||
      !animrig::legacy::assigned_action_has_keyframes(ob->adt))
  {
    /* Default to the scene (preview) range if there is no animation data to
     * find selected keys in. */
    avs->path_sf = scene->playback_start();
    avs->path_ef = scene->playback_end();
    return;
  }

  AnimKeylist *keylist = ED_keylist_create();
  for (FCurve *fcu : animrig::fcurves_for_assigned_action(ob->adt)) {
    fcurve_to_keylist(ob->adt, fcu, keylist, 0, {-FLT_MAX, FLT_MAX}, true);
  }

  Bounds<float> frame_range;
  switch (avs->path_range) {
    case MOTIONPATH_RANGE_KEYS_SELECTED:
      if (ED_keylist_selected_keys_frame_range(keylist, &frame_range)) {
        break;
      }
      ATTR_FALLTHROUGH; /* Fall through if there were no selected keys found. */
    case MOTIONPATH_RANGE_KEYS_ALL:
      ED_keylist_all_keys_frame_range(keylist, &frame_range);
      break;
    case MOTIONPATH_RANGE_MANUAL:
    case MOTIONPATH_RANGE_SCENE:
      BLI_assert_msg(false, "This should not happen, function should have exited earlier.");
  };

  avs->path_sf = frame_range.min;
  avs->path_ef = frame_range.max;

  ED_keylist_free(keylist);
}

static void build_keylist_for_target(MPathTarget &target, AnimKeylist &keylist)
{
  /* For object level motion paths this is a nullptr in which case the filtering is ignored. */
  bPoseChannel *pose_bone = target.pchan;
  for (FCurve *fcu : animrig::fcurves_for_assigned_action(target.ob->adt)) {
    if (pose_bone &&
        !animrig::fcurve_matches_collection_path(*fcu, "pose.bones[", pose_bone->name))
    {
      continue;
    }
    /* When only updating a subset of the motion path we could pass a range here to improve
     * performance. */
    fcurve_to_keylist(target.ob->adt, fcu, &keylist, 0, {-FLT_MAX, FLT_MAX}, true);
  }
}

static bool are_all_verts_evaluated(bMotionPath &mpath)
{
  for (int i = 0; i < mpath.length; i++) {
    if (!(mpath.points[i].flag & MOTIONPATH_VERT_EVALUATED)) {
      return false;
    }
  }

  return true;
}

static bool update_callback(
    Depsgraph &dg, ID &orig_id, ID &eval_id, const StringRef /* component_name */, const int frame)
{
  Object *ob = id_cast<Object *>(&orig_id);
  Object *ob_eval = id_cast<Object *>(&eval_id);
  bMotionPath *mpath = ob->mpath;

  if (!mpath) {
    /* If the motion path was deleted, signal that this evaluation can end. */
    return true;
  }

  if ((frame < mpath->start_frame) || (frame >= mpath->end_frame)) {
    return are_all_verts_evaluated(*mpath);
  }

  const int index = frame - mpath->start_frame;
  BLI_assert(index >= 0 && index < mpath->length);
  bMotionPathVert &mpv = mpath->points[index];

  /* World-space object location. */
  copy_v3_v3(mpv.co, ob_eval->object_to_world().location());
  if (mpath->flag & MOTIONPATH_FLAG_BAKE_CAMERA) {
    transform_mpath_point_to_active_camera(dg, mpv);
  }

  mpv.flag |= MOTIONPATH_VERT_EVALUATED;

  if (animrig::id_frame_has_keyframe(&ob->id, frame)) {
    mpv.flag |= MOTIONPATH_VERT_KEY;
  }
  else {
    mpv.flag &= ~MOTIONPATH_VERT_KEY;
  }

  DEG_id_tag_update(&ob->id, ID_RECALC_ANIMATION_NO_FLUSH);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW_ANIMVIZ, ob);

  return are_all_verts_evaluated(*mpath);
}

static bool update_callback_pose_bone(
    Depsgraph &dg, ID &orig_id, ID &eval_id, const StringRef component_name, const int frame)
{
  Object *ob = id_cast<Object *>(&orig_id);
  Object *ob_eval = id_cast<Object *>(&eval_id);

  bPoseChannel *pose_bone = BKE_pose_channel_find_name(ob->pose, component_name.data());
  bPoseChannel *pose_bone_eval = BKE_pose_channel_find_name(ob_eval->pose, component_name.data());

  if (!pose_bone || !pose_bone->mpath) {
    /* Pose bone or its motion path was deleted. */
    return true;
  }

  bMotionPath *mpath = pose_bone->mpath;

  if ((frame < mpath->start_frame) || (frame >= mpath->end_frame)) {
    return are_all_verts_evaluated(*mpath);
  }

  const int index = frame - mpath->start_frame;
  BLI_assert(index >= 0 && index < mpath->length);
  bMotionPathVert &mpv = mpath->points[index];

  if (mpath->flag & MOTIONPATH_FLAG_BHEAD) {
    copy_v3_v3(mpv.co, pose_bone_eval->pose_head);
  }
  else {
    copy_v3_v3(mpv.co, pose_bone_eval->pose_tail);
  }

  /* Result must be in world-space. */
  mul_m4_v3(ob_eval->object_to_world().ptr(), mpv.co);
  if (mpath->flag & MOTIONPATH_FLAG_BAKE_CAMERA) {
    transform_mpath_point_to_active_camera(dg, mpv);
  }

  mpv.flag |= MOTIONPATH_VERT_EVALUATED;
  if (animrig::bone_frame_has_keyframe(*ob, pose_bone->name, frame)) {
    mpv.flag |= MOTIONPATH_VERT_KEY;
  }
  else {
    mpv.flag &= ~MOTIONPATH_VERT_KEY;
  }

  DEG_id_tag_update(&ob->id, ID_RECALC_ANIMATION_NO_FLUSH);
  WM_main_add_notifier(NC_OBJECT | ND_DRAW_ANIMVIZ, ob);

  return are_all_verts_evaluated(*mpath);
}

/**
 * Returns which parts of the FCurve would be affected by a change at the given frame.
 */
static Bounds<int> get_affected_range(const FCurve &fcurve, const float frame)
{
  if (!fcurve.bezt) {
    return {int(frame), int(frame) + 1};
  }

  bool is_on_key;
  const int index = BKE_fcurve_bezt_binarysearch_index(
      fcurve.bezt, frame, fcurve.totvert, &is_on_key);

  const int index_distance = fcurve.auto_smoothing == FCURVE_SMOOTH_NONE ? 2 : 4;

  if (is_on_key) {
    const BezTriple &lower_bound = fcurve.bezt[max_ii(index - index_distance, 0)];
    const BezTriple &upper_bound = fcurve.bezt[min_ii(index + index_distance, fcurve.totvert - 1)];
    return {int(lower_bound.vec[1][0]), int(upper_bound.vec[1][0])};
  }

  const BezTriple &lower_bound = fcurve.bezt[max_ii(index - 1 - index_distance, 0)];
  const BezTriple &upper_bound = fcurve.bezt[min_ii(index + index_distance, fcurve.totvert - 1)];
  return {int(lower_bound.vec[1][0]), int(upper_bound.vec[1][0])};
}

Bounds<int> animviz_get_affected_edit_range(const Object &object, const float modified_frame)
{
  Bounds<int> bounds = {int(modified_frame), int(modified_frame) + 1};
  if (!object.adt || !object.adt->action || object.adt->slot_handle == animrig::Slot::unassigned) {
    return bounds;
  }

  for (FCurve *fcu : animrig::fcurves_for_assigned_action(object.adt)) {
    if (!fcu->bezt) {
      continue;
    }
    bounds = bounds::merge(bounds, get_affected_range(*fcu, modified_frame));
  }

  return bounds;
}

Bounds<int> animviz_get_affected_edit_range(const Object &object,
                                            const bPoseChannel &pose_bone,
                                            const float modified_frame)
{
  Bounds<int> bounds = {int(modified_frame), int(modified_frame) + 1};
  if (!object.adt || !object.adt->action || object.adt->slot_handle == animrig::Slot::unassigned) {
    return bounds;
  }

  for (FCurve *fcu : animrig::fcurves_for_assigned_action(object.adt)) {
    if (!fcu->bezt) {
      continue;
    }
    if (!animrig::fcurve_matches_collection_path(*fcu, "pose.bones[", pose_bone.name)) {
      continue;
    }
    bounds = bounds::merge(bounds, get_affected_range(*fcu, modified_frame));
  }

  return bounds;
}

void animviz_tag_for_motion_path_eval(wmWindow &window, Object &object, Bounds<int> range)
{
  BLI_assert(window.runtime != nullptr);
  bMotionPath *mpath = object.mpath;
  if (!mpath) {
    BLI_assert_unreachable();
    return;
  }
  for (int i = 0; i < mpath->length; i++) {
    mpath->points[i].flag &= ~MOTIONPATH_VERT_EVALUATED;
  }
  if (range.is_empty()) {
    range = {mpath->start_frame, mpath->end_frame};
  }

  bke::wm_staggered_eval_register(*window.runtime, object.id, "", range, update_callback);
}

void animviz_tag_for_motion_path_eval(wmWindow &window,
                                      Object &armature_object,
                                      bPoseChannel &pose_bone,
                                      Bounds<int> range)
{
  bMotionPath *mpath = pose_bone.mpath;
  if (!mpath) {
    BLI_assert_unreachable();
    return;
  }
  for (int i = 0; i < mpath->length; i++) {
    mpath->points[i].flag &= ~MOTIONPATH_VERT_EVALUATED;
  }
  if (range.is_empty()) {
    range = {mpath->start_frame, mpath->end_frame};
  }

  bke::wm_staggered_eval_register(
      *window.runtime, armature_object.id, pose_bone.name, range, update_callback_pose_bone);
}

void animviz_calc_motionpaths(Depsgraph *depsgraph,
  Scene *scene,
                              MutableSpan<MPathTarget> targets,
                              const Bounds<int> frame_range)
{
  using namespace blender::animrig;
  BLI_assert_msg(!DEG_is_active(depsgraph),
                 "Motion path calculation should always happen with a minimal depsgraph.");

  if (targets.is_empty() || frame_range.is_empty()) {
    return;
  }

  for (MPathTarget &mpt : targets) {
    AnimData *adt = BKE_animdata_from_id(&mpt.ob->id);

    /* Build list of all keyframes in active action for object or pchan. */
    mpt.keylist = ED_keylist_create();
    mpt.keylist = ED_keylist_create();

    Vector<FCurve *> fcurves;
    if (adt && adt->action) {
      /* Get pointer to animviz settings for each target. */
      bAnimVizSettings *avs = animviz_target_settings_get(mpt);

      /* For bones it is likely that all FCurves belong to a group named after the bone. Only
       * checking FCurves of a given group can improve performance when building the keylist. */
      if ((mpt.pchan) && (avs->path_viewflag & MOTIONPATH_VIEW_KFACT) == 0) {
        if ((mpt.pchan) && (avs->path_viewflag & MOTIONPATH_VIEW_KFACT) == 0) {
          Action &action = adt->action->wrap();
          bActionGroup *agrp = nullptr;
          Channelbag *cbag = channelbag_for_action_slot(action, adt->slot_handle);
          agrp = cbag ? cbag->channel_group_find(mpt.pchan->name) : nullptr;
          agrp = cbag ? cbag->channel_group_find(mpt.pchan->name) : nullptr;

          if (agrp) {
            fcurves = listbase_to_vector<FCurve>(agrp->channels);
            action_group_to_keylist(adt, agrp, mpt.keylist, 0, {-FLT_MAX, FLT_MAX});
            action_group_to_keylist(adt, agrp, mpt.keylist, 0, {-FLT_MAX, FLT_MAX});
          }
        }
        else {
          build_keylist_for_target(mpt, *mpt.keylist);
          build_keylist_for_target(mpt, *mpt.keylist);
        }
      }
      ED_keylist_prepare_for_direct_access(mpt.keylist);
    }

    /* Calculate path over requested range. */
    CLOG_INFO(&LOG,
              "Calculating MotionPaths between frames %d - %d (%d frames)",
              frame_range.min,
              frame_range.max,
              frame_range.max - frame_range.min + 1);

  for (int frame = frame_range.min; frame < frame_range.max; frame++) {
    /* Update relevant data for new frame. */
    DEG_evaluate_on_framechange(depsgraph, frame);

    /* Perform baking for targets. */
    for (const MPathTarget &target : targets) {
      motionpaths_calc_bake_target(target, frame, depsgraph, scene->camera);
    }
  }

    /* Clear recalc flags from targets. */
    for (MPathTarget &mpt : targets) {
      /* Get pointer to animviz settings for each target. */
      bAnimVizSettings *avs = animviz_target_settings_get(mpt);

      /* Clear the flag requesting recalculation of targets. */
      avs->recalc &= ~ANIMVIZ_RECALC_PATHS;

      /* Clean temp data. */
      ED_keylist_free(mpt.keylist);
      DEG_id_tag_update(&mpt.ob->id, ID_RECALC_ANIMATION_NO_FLUSH);
      WM_main_add_notifier(NC_OBJECT | ND_DRAW_ANIMVIZ, mpt.ob);
    }
  }
}

}  // namespace blender
