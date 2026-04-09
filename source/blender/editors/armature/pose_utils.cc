/* SPDX-FileCopyrightText: 2009 Blender Authors, Joshua Leung.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edarmature
 */

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_object_types.h"

#include "BKE_action.hh"
#include "BKE_anim_data.hh"
#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_object.hh"

#include "BKE_context.hh"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_path.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_armature.hh"
#include "ED_keyframing.hh"

#include "ANIM_action.hh"
#include "ANIM_action_iterators.hh"
#include "ANIM_keyframing.hh"
#include "ANIM_keyingsets.hh"
#include "ANIM_rna.hh"
#include "ANIM_transformable.hh"

#include "armature_intern.hh"

namespace blender {

/* *********************************************** */
/* Contents of this File:
 *
 * This file contains methods shared between Pose Slide and Pose Lib;
 * primarily the functions in question concern Animato <-> Pose
 * convenience functions, such as applying/getting pose values
 * and/or inserting keyframes for these.
 */
/* *********************************************** */
/* FCurves <-> PoseChannels Links */

/**
 * Types of transforms applied to the given item:
 * - these are the return flags for get_item_transform_flags()
 */

static eAction_TransformFlags get_item_transform_flags_and_fcurves(ID &id,
                                                                   PointerRNA &ptr,
                                                                   Vector<FCurve *> &r_curves)
{
  AnimData *adt = BKE_animdata_from_id(&id);
  if (!adt || !adt->action) {
    return eAction_TransformFlags(0);
  }
  animrig::Action &action = adt->action->wrap();

  short flags = 0;

  /* Get the basic path to the properties of interest. */
  const std::optional<std::string> path_to_struct = RNA_path_from_ID_to_struct(&ptr);
  StringRef base_path;
  if (RNA_struct_is_ID(ptr.type)) {
    base_path = "";
  }
  else {
    if (!path_to_struct.has_value()) {
      BLI_assert_unreachable();
      return eAction_TransformFlags(0);
    }
    base_path = path_to_struct.value();
  }

  animrig::foreach_fcurve_in_action_slot(action, adt->slot_handle, [&](FCurve &fcurve) {
    if (fcurve.rna_path == nullptr) {
      return;
    }
    StringRefNull fcurve_path(fcurve.rna_path);

    if (!base_path.is_empty() && !fcurve_path.startswith(base_path)) {
      return;
    }

    StringRef property_name;
    if (base_path.is_empty()) {
      property_name = fcurve_path;
    }
    else {
      /* Normal properties are separated by a dot, custom properties don't have that. */
      if (fcurve_path[base_path.size()] == '.') {
        property_name = fcurve_path.substr(base_path.size() + 1);
      }
      else {
        property_name = fcurve_path.substr(base_path.size());
      }
    }

    if (property_name == "location") {
      flags |= ACT_TRANS_LOC;
      r_curves.append(&fcurve);
      return;
    }

    if (property_name == "scale") {
      flags |= ACT_TRANS_SCALE;
      r_curves.append(&fcurve);
      return;
    }

    if (property_name == "rotation_euler" || property_name == "rotation_quaternion" ||
        property_name == "rotation_axis_angle")
    {
      flags |= ACT_TRANS_ROT;
      r_curves.append(&fcurve);
      return;
    }

    if (property_name.startswith("bbone_")) {
      flags |= ACT_TRANS_BBONE;
      r_curves.append(&fcurve);
      return;
    }

    /* Custom properties only. */
    if (property_name.startswith("[\"")) {
      flags |= ACT_TRANS_PROP;
      r_curves.append(&fcurve);
      return;
    }
  });

  /* return flags found */
  return eAction_TransformFlags(flags);
}

/**
 * Stores a `PropertySnapshot` of the property with the given property name in the given vector. If
 * the property does not exist in the `ptr` the function doesn't do anything.
 * Also the property has to be supported by `animrig::rna_property_get_as_float`.
 */
static void store_property_snapshot(PointerRNA &ptr,
                                    StringRef property_name,
                                    Vector<PropertySnapshot> &snapshots)
{
  PropertyRNA *prop = RNA_struct_find_property(&ptr, property_name.data());
  if (!prop) {
    return;
  }
  Array<float> property_values = animrig::rna_property_get_as_float(ptr, *prop);
  if (property_values.size() == 0) {
    /* Unsupported property type. */
    return;
  }
  snapshots.append({prop, std::move(property_values)});
}

/* helper for poseAnim_mapping_get() -> get the relevant F-Curves per PoseChannel */
static void fcurves_to_pchan_links_get(ListBaseT<TransformableFCurveLink> &pfLinks,
                                       Object &ob,
                                       bPoseChannel &pchan)
{
  PointerRNA bone_ptr = RNA_pointer_create_discrete(&ob.id, RNA_PoseBone, &pchan);
  Vector<FCurve *> curves;
  const eAction_TransformFlags transFlags = get_item_transform_flags_and_fcurves(
      ob.id, bone_ptr, curves);

  if (!transFlags) {
    return;
  }

  TransformableFCurveLink *pfl = MEM_new<TransformableFCurveLink>("tPChanFCurveLink");
  BLI_addtail(&pfLinks, pfl);
  pfl->fcurves = curves;

  animrig::Transformable *transformable = MEM_new<animrig::Transformable>(
      "transformable_pose_bone", ob, pchan);
  pfl->transformable = transformable;

  /* Set pchan's transform flags. */
  pfl->transform_flag = transFlags;

  pfl->old_loc = transformable->get_property(animrig::Transformable::PropertyType::LOCATION);
  pfl->old_rot = transformable->get_rotation();
  pfl->old_scale = transformable->get_property(animrig::Transformable::PropertyType::SCALE);

  pfl->ptr = bone_ptr;

  /* Store current bbone values. */
  if (transFlags & ACT_TRANS_BBONE) {
    store_property_snapshot(bone_ptr, "bbone_rollin", pfl->additional_properties);
    store_property_snapshot(bone_ptr, "bbone_rollout", pfl->additional_properties);
    store_property_snapshot(bone_ptr, "bbone_curveinx", pfl->additional_properties);
    store_property_snapshot(bone_ptr, "bbone_curveoutx", pfl->additional_properties);
    store_property_snapshot(bone_ptr, "bbone_curveinz", pfl->additional_properties);
    store_property_snapshot(bone_ptr, "bbone_curveoutz", pfl->additional_properties);
    store_property_snapshot(bone_ptr, "bbone_easein", pfl->additional_properties);
    store_property_snapshot(bone_ptr, "bbone_easeout", pfl->additional_properties);
    store_property_snapshot(bone_ptr, "bbone_scalein", pfl->additional_properties);
    store_property_snapshot(bone_ptr, "bbone_scaleout", pfl->additional_properties);
  }

  /* Make copy of custom properties. */
  if (transFlags & ACT_TRANS_PROP) {
    PropertyRNA *prop;
    if (pchan.prop) {
      for (const IDProperty &id_prop : pchan.prop->data.group) {
        if (ELEM(id_prop.type, IDP_STRING, IDP_ID, IDP_IDPARRAY)) {
          continue;
        }
        char name_escaped[MAX_IDPROP_NAME * 2];
        BLI_str_escape(name_escaped, id_prop.name, sizeof(name_escaped));
        std::string path = fmt::format("[\"{}\"]", name_escaped);
        store_property_snapshot(bone_ptr, path, pfl->custom_properties);
      }
    }
    if (pchan.system_properties) {
      for (const IDProperty &id_prop : pchan.prop->data.group) {
        if (ELEM(id_prop.type, IDP_STRING, IDP_ID, IDP_IDPARRAY)) {
          continue;
        }
        store_property_snapshot(bone_ptr, id_prop.name, pfl->custom_properties);
      }
    }
  }
}

Object *poseAnim_object_get(Object *ob_)
{
  Object *ob = BKE_object_pose_armature_get(ob_);
  if (!ELEM(nullptr, ob, ob->data, ob->adt, ob->adt->action)) {
    return ob;
  }
  return nullptr;
}

static void get_pose_bones_for_slide(bContext *C, ListBaseT<TransformableFCurveLink> &pfLinks)
{
  /* For each Pose-Channel which gets affected, get the F-Curves for that channel
   * and set the relevant transform flags... */
  Object *prev_ob, *ob_pose_armature;

  prev_ob = nullptr;
  ob_pose_armature = nullptr;
  CTX_DATA_BEGIN_WITH_ID (C, bPoseChannel *, pchan, selected_pose_bones, Object *, ob) {
    BLI_assert(pchan != nullptr);
    if (ob != prev_ob) {
      prev_ob = ob;
      ob_pose_armature = poseAnim_object_get(ob);
    }

    if (ob_pose_armature == nullptr) {
      continue;
    }
    if (!ob_pose_armature->adt || !ob_pose_armature->adt->action) {
      /* No action means no FCurves. */
      continue;
    }

    fcurves_to_pchan_links_get(pfLinks, *ob_pose_armature, *pchan);
  }
  CTX_DATA_END;

  /* If no PoseChannels were found, try a second pass, doing visible ones instead.
   * i.e. if nothing selected, do whole pose.
   */
  if (BLI_listbase_is_empty(&pfLinks)) {
    prev_ob = nullptr;
    ob_pose_armature = nullptr;
    CTX_DATA_BEGIN_WITH_ID (C, bPoseChannel *, pchan, visible_pose_bones, Object *, ob) {
      BLI_assert(pchan != nullptr);
      if (ob != prev_ob) {
        prev_ob = ob;
        ob_pose_armature = poseAnim_object_get(ob);
      }

      if (ob_pose_armature == nullptr) {
        continue;
      }
      if (!ob_pose_armature->adt || !ob_pose_armature->adt->action) {
        /* No action means no FCurves. */
        continue;
      }

      fcurves_to_pchan_links_get(pfLinks, *ob_pose_armature, *pchan);
    }
    CTX_DATA_END;
  }
}

void poseAnim_mapping_get(bContext *C, ListBaseT<TransformableFCurveLink> *pfLinks)
{
  BLI_assert(pfLinks != nullptr);
  const eContextObjectMode mode = CTX_data_mode_enum(C);
  switch (mode) {
    case CTX_MODE_POSE:
      get_pose_bones_for_slide(C, *pfLinks);
      break;

    default:
      /* Not implemented. */
      BLI_assert_unreachable();
      break;
  }
}

void poseAnim_mapping_free(ListBaseT<TransformableFCurveLink> *pfLinks)
{
  TransformableFCurveLink *pfl, *pfln = nullptr;

  /* free the temp pchan links and their data */
  for (pfl = static_cast<TransformableFCurveLink *>(pfLinks->first); pfl; pfl = pfln) {
    pfln = pfl->next;

    MEM_delete(pfl->transformable);

    /* We cannot use BLI_freelinkN because that casts the TransformableFCurveLink to a C-style
     * struct causing MEM_delete to do a C-style delete and not deallocating the Vector. */
    BLI_remlink(pfLinks, pfl);
    MEM_delete(pfl);
  }
}

/* ------------------------- */

void poseAnim_mapping_refresh(bContext *C, ID *id)
{
  DEG_id_tag_update(id, ID_RECALC_GEOMETRY);
  DEG_id_tag_update(id, ID_RECALC_TRANSFORM);
  switch (GS(id->name)) {
    case ID_OB:
      WM_event_add_notifier(C, NC_OBJECT | ND_POSE | ND_TRANSFORM, id_cast<Object *>(id));
      break;
    default:
      /* Not implemented. */
      BLI_assert_unreachable();
      break;
  }

  AnimData *adt = BKE_animdata_from_id(id);
  if (adt && adt->action) {
    DEG_id_tag_update(&adt->action->id, ID_RECALC_ANIMATION_NO_FLUSH);
  }
}

void poseAnim_mapping_reset(ListBaseT<TransformableFCurveLink> *pfLinks)
{
  /* Iterate over each transformable affected, restoring all channels to their original values. */
  for (TransformableFCurveLink &pfl : *pfLinks) {
    animrig::Transformable *transformable = pfl.transformable;

    /* just copy all the values over regardless of whether they changed or not */
    transformable->set_property(
        animrig::Transformable::PropertyType::LOCATION, pfl.old_loc, animrig::AXIS_FLAG_NONE);
    transformable->set_rotation(pfl.old_rot);
    transformable->set_property(
        animrig::Transformable::PropertyType::SCALE, pfl.old_scale, animrig::AXIS_FLAG_NONE);

    for (PropertySnapshot &extra_prop : pfl.additional_properties) {
      animrig::rna_property_set_as_float(pfl.ptr, *extra_prop.property, extra_prop.backup_values);
    }

    for (PropertySnapshot &custom_prop : pfl.custom_properties) {
      animrig::rna_property_set_as_float(
          pfl.ptr, *custom_prop.property, custom_prop.backup_values);
    }
  }
}

void poseAnim_mapping_autoKeyframe(bContext *C,
                                   Scene *scene,
                                   ListBaseT<TransformableFCurveLink> *pfLinks,
                                   float cframe)
{
  const Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  View3D *v3d = CTX_wm_view3d(C);
  bool skip = true;

  FOREACH_OBJECT_IN_MODE_BEGIN (bmain, scene, view_layer, v3d, OB_ARMATURE, OB_MODE_POSE, ob) {
    ob->id.tag &= ~ID_TAG_DOIT;
    ob = poseAnim_object_get(ob);

    /* Ensure validity of the settings from the context. */
    if (ob == nullptr) {
      continue;
    }

    if (animrig::autokeyframe_cfra_can_key(scene, &ob->id)) {
      ob->id.tag |= ID_TAG_DOIT;
      skip = false;
    }
  }
  FOREACH_OBJECT_IN_MODE_END;

  if (skip) {
    return;
  }

  /* Insert keyframes as necessary if auto-key-framing. */
  KeyingSet *ks = animrig::get_keyingset_for_autokeying(scene, ANIM_KS_WHOLE_CHARACTER_ID);
  Vector<PointerRNA> sources;

  /* iterate over each pose-channel affected, tagging bones to be keyed */
  /* XXX: here we already have the information about what transforms exist, though
   * it might be easier to just overwrite all using normal mechanisms
   */
  for (TransformableFCurveLink &pfl : *pfLinks) {
    animrig::Transformable *transformable = pfl.transformable;
    bPoseChannel *pchan = static_cast<bPoseChannel *>(transformable->data());

    if ((transformable->owner_id()->tag & ID_TAG_DOIT) == 0) {
      continue;
    }

    /* Add data-source override for the PoseChannel, to be used later. */
    animrig::relative_keyingset_add_source(
        sources, transformable->owner_id(), RNA_PoseBone, pchan);
  }

  /* insert keyframes for all relevant bones in one go */
  animrig::apply_keyingset(C, &sources, ks, animrig::ModifyKeyMode::INSERT, cframe);

  /* do the bone paths
   * - only do this if keyframes should have been added
   * - do not calculate unless there are paths already to update...
   */
  FOREACH_OBJECT_IN_MODE_BEGIN (bmain, scene, view_layer, v3d, OB_ARMATURE, OB_MODE_POSE, ob) {
    if (ob->id.tag & ID_TAG_DOIT) {
      if (ob->pose->avs.path_bakeflag & MOTIONPATH_BAKE_HAS_PATHS) {
        // ED_pose_clear_paths(C, ob); /* XXX for now, don't need to clear. */
        /* TODO(sergey): Should ensure we can use more narrow update range here. */
        ED_pose_recalculate_paths(C, scene, ob, POSE_PATH_CALC_RANGE_FULL);
      }
    }
  }
  FOREACH_OBJECT_IN_MODE_END;
}

/* *********************************************** */

}  // namespace blender
