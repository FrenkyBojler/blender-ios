/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edobj
 */

#include <cstdlib>
#include <cstring>
#include <numeric>

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_collection_types.h"
#include "DNA_grease_pencil_types.h"
#include "DNA_lattice_types.h"
#include "DNA_light_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meta_types.h"
#include "DNA_object_types.h"
#include "DNA_pointcloud_types.h"
#include "DNA_scene_types.h"

#include "BLI_array.hh"
#include "BLI_listbase.h"
#include "BLI_math_base_safe.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_armature.hh"
#include "BKE_context.hh"
#include "BKE_curve.hh"
#include "BKE_curves.hh"
#include "BKE_editmesh.hh"
#include "BKE_grease_pencil.hh"
#include "BKE_idtype.hh"
#include "BKE_lattice.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_mball.hh"
#include "BKE_mesh.hh"
#include "BKE_multires.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_tracking.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "UI_interface.hh"
#include "UI_interface_icons.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ANIM_keyframing.hh"
#include "ANIM_keyingsets.hh"

#include "ED_anim_api.hh"
#include "ED_armature.hh"
#include "ED_mesh.hh"
#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_transform_snap_object_context.hh"
#include "ED_view3d.hh"

#include "MEM_guardedalloc.h"

#include "RNA_prototypes.hh"

#include "object_intern.hh"

namespace blender::ed::object {

/* -------------------------------------------------------------------- */
/** \name Precision Mode Utilities
 * \{ */

/**
 * Calculate effective mouse position with precision mode support.
 * Applies precision factor to mouse movement delta from precision toggle position.
 */
static void precision_mode_mouse_pos(bool precision_mode,
                                     float precision_factor,
                                     const int current_mval[2],
                                     const int precision_toggle_mval[2],
                                     int effective_mval[2])
{
  if (precision_mode) {
    /* Apply precision factor to mouse movement delta from when precision was enabled */
    float delta_x = (current_mval[0] - precision_toggle_mval[0]) * precision_factor;
    float delta_y = (current_mval[1] - precision_toggle_mval[1]) * precision_factor;
    effective_mval[0] = int(precision_toggle_mval[0] + delta_x);
    effective_mval[1] = int(precision_toggle_mval[1] + delta_y);
  }
  else {
    effective_mval[0] = current_mval[0];
    effective_mval[1] = current_mval[1];
  }
}

/** \} */

/**
 * Compute a smoothed surface normal at mouse position using neighboring pixels.
 * Used by light positioning operators and transform axis target.
 */
static bool get_smoothed_surface_normal(const ViewContext *vc,
                                        const ViewDepths *depths,
                                        const int mval[2],
                                        float normal[3])
{
  if (!depths) {
    return false;
  }

  /* Read normal at cursor position */
  if (!ED_view3d_depth_read_cached_normal(vc->region, depths, mval, normal)) {
    return false;
  }

  /* Apply grid-based smoothing - same algorithm as Ctrl+Alt transform */
  constexpr int ofs = 2;
  for (int x = -ofs; x <= ofs; x += ofs / 2) {
    for (int y = -ofs; y <= ofs; y += ofs / 2) {
      if (x != 0 && y != 0) {
        const int mval_ofs[2] = {mval[0] + x, mval[1] + y};
        float normal_ofs[3];
        if (ED_view3d_depth_read_cached_normal(vc->region, depths, mval_ofs, normal_ofs)) {
          add_v3_v3(normal, normal_ofs);
        }
      }
    }
  }

  normalize_v3(normal);
  return true;
}

/**
 * Perform auto-keying for object rotation changes based on the object's rotation mode.
 */
static void autokeyframe_object_rotation(bContext *C, Scene *scene, Object *ob)
{
  PointerRNA ptr = RNA_pointer_create_discrete(&ob->id, RNA_Object, &ob->id);
  const char *rotation_property = "rotation_euler";
  switch (ob->rotmode) {
    case ROT_MODE_QUAT:
      rotation_property = "rotation_quaternion";
      break;
    case ROT_MODE_AXISANGLE:
      rotation_property = "rotation_axis_angle";
      break;
    default:
      break;
  }
  PropertyRNA *prop = RNA_struct_find_property(&ptr, rotation_property);
  animrig::autokeyframe_property(C, scene, &ptr, prop, -1, scene->r.cfra, true);
}

/**
 * Position light along surface normal with given offset distance.
 * Uses legacy vector functions for consistency with existing codebase.
 */
static void position_light_along(const float normal[3],
                                 const float location_world[3],
                                 float offset_distance,
                                 float direction[3],
                                 float final_normal[3])
{
  copy_v3_v3(final_normal, normal);
  copy_v3_v3(direction, location_world);
  madd_v3_v3fl(direction, final_normal, offset_distance);
}

/* -------------------------------------------------------------------- */
/** \name Clear Transformation Utilities
 * \{ */

/* clear location of object */
static void object_clear_loc(Object *ob, const bool clear_delta)
{
  /* clear location if not locked */
  if ((ob->protectflag & OB_LOCK_LOCX) == 0) {
    ob->loc[0] = 0.0f;
    if (clear_delta) {
      ob->dloc[0] = 0.0f;
    }
  }
  if ((ob->protectflag & OB_LOCK_LOCY) == 0) {
    ob->loc[1] = 0.0f;
    if (clear_delta) {
      ob->dloc[1] = 0.0f;
    }
  }
  if ((ob->protectflag & OB_LOCK_LOCZ) == 0) {
    ob->loc[2] = 0.0f;
    if (clear_delta) {
      ob->dloc[2] = 0.0f;
    }
  }
}

/* clear rotation of object */
static void object_clear_rot(Object *ob, const bool clear_delta)
{
  /* clear rotations that aren't locked */
  if (ob->protectflag & (OB_LOCK_ROTX | OB_LOCK_ROTY | OB_LOCK_ROTZ | OB_LOCK_ROTW)) {
    if (ob->protectflag & OB_LOCK_ROT4D) {
      /* perform clamping on a component by component basis */
      if (ob->rotmode == ROT_MODE_AXISANGLE) {
        if ((ob->protectflag & OB_LOCK_ROTW) == 0) {
          ob->rotAngle = 0.0f;
          if (clear_delta) {
            ob->drotAngle = 0.0f;
          }
        }
        if ((ob->protectflag & OB_LOCK_ROTX) == 0) {
          ob->rotAxis[0] = 0.0f;
          if (clear_delta) {
            ob->drotAxis[0] = 0.0f;
          }
        }
        if ((ob->protectflag & OB_LOCK_ROTY) == 0) {
          ob->rotAxis[1] = 0.0f;
          if (clear_delta) {
            ob->drotAxis[1] = 0.0f;
          }
        }
        if ((ob->protectflag & OB_LOCK_ROTZ) == 0) {
          ob->rotAxis[2] = 0.0f;
          if (clear_delta) {
            ob->drotAxis[2] = 0.0f;
          }
        }

        /* Check validity of axis - axis should never be 0,0,0
         * (if so, then we make it rotate about y). */
        if (IS_EQF(ob->rotAxis[0], ob->rotAxis[1]) && IS_EQF(ob->rotAxis[1], ob->rotAxis[2])) {
          ob->rotAxis[1] = 1.0f;
        }
        if (IS_EQF(ob->drotAxis[0], ob->drotAxis[1]) && IS_EQF(ob->drotAxis[1], ob->drotAxis[2]) &&
            clear_delta)
        {
          ob->drotAxis[1] = 1.0f;
        }
      }
      else if (ob->rotmode == ROT_MODE_QUAT) {
        if ((ob->protectflag & OB_LOCK_ROTW) == 0) {
          ob->quat[0] = 1.0f;
          if (clear_delta) {
            ob->dquat[0] = 1.0f;
          }
        }
        if ((ob->protectflag & OB_LOCK_ROTX) == 0) {
          ob->quat[1] = 0.0f;
          if (clear_delta) {
            ob->dquat[1] = 0.0f;
          }
        }
        if ((ob->protectflag & OB_LOCK_ROTY) == 0) {
          ob->quat[2] = 0.0f;
          if (clear_delta) {
            ob->dquat[2] = 0.0f;
          }
        }
        if ((ob->protectflag & OB_LOCK_ROTZ) == 0) {
          ob->quat[3] = 0.0f;
          if (clear_delta) {
            ob->dquat[3] = 0.0f;
          }
        }
        /* TODO: does this quat need normalizing now? */
      }
      else {
        /* the flag may have been set for the other modes, so just ignore the extra flag... */
        if ((ob->protectflag & OB_LOCK_ROTX) == 0) {
          ob->rot[0] = 0.0f;
          if (clear_delta) {
            ob->drot[0] = 0.0f;
          }
        }
        if ((ob->protectflag & OB_LOCK_ROTY) == 0) {
          ob->rot[1] = 0.0f;
          if (clear_delta) {
            ob->drot[1] = 0.0f;
          }
        }
        if ((ob->protectflag & OB_LOCK_ROTZ) == 0) {
          ob->rot[2] = 0.0f;
          if (clear_delta) {
            ob->drot[2] = 0.0f;
          }
        }
      }
    }
    else {
      /* perform clamping using euler form (3-components) */
      /* FIXME: deltas are not handled for these cases yet... */
      float eul[3], oldeul[3], quat1[4] = {0};

      if (ob->rotmode == ROT_MODE_QUAT) {
        copy_qt_qt(quat1, ob->quat);
        quat_to_eul(oldeul, ob->quat);
      }
      else if (ob->rotmode == ROT_MODE_AXISANGLE) {
        axis_angle_to_eulO(oldeul, EULER_ORDER_DEFAULT, ob->rotAxis, ob->rotAngle);
      }
      else {
        copy_v3_v3(oldeul, ob->rot);
      }

      eul[0] = eul[1] = eul[2] = 0.0f;

      if (ob->protectflag & OB_LOCK_ROTX) {
        eul[0] = oldeul[0];
      }
      if (ob->protectflag & OB_LOCK_ROTY) {
        eul[1] = oldeul[1];
      }
      if (ob->protectflag & OB_LOCK_ROTZ) {
        eul[2] = oldeul[2];
      }

      if (ob->rotmode == ROT_MODE_QUAT) {
        eul_to_quat(ob->quat, eul);
        /* quaternions flip w sign to accumulate rotations correctly */
        if ((quat1[0] < 0.0f && ob->quat[0] > 0.0f) || (quat1[0] > 0.0f && ob->quat[0] < 0.0f)) {
          mul_qt_fl(ob->quat, -1.0f);
        }
      }
      else if (ob->rotmode == ROT_MODE_AXISANGLE) {
        eulO_to_axis_angle(ob->rotAxis, &ob->rotAngle, eul, EULER_ORDER_DEFAULT);
      }
      else {
        copy_v3_v3(ob->rot, eul);
      }
    }
  } /* Duplicated in source/blender/editors/armature/editarmature.c */
  else {
    if (ob->rotmode == ROT_MODE_QUAT) {
      unit_qt(ob->quat);
      if (clear_delta) {
        unit_qt(ob->dquat);
      }
    }
    else if (ob->rotmode == ROT_MODE_AXISANGLE) {
      unit_axis_angle(ob->rotAxis, &ob->rotAngle);
      if (clear_delta) {
        unit_axis_angle(ob->drotAxis, &ob->drotAngle);
      }
    }
    else {
      zero_v3(ob->rot);
      if (clear_delta) {
        zero_v3(ob->drot);
      }
    }
  }
}

/* clear scale of object */
static void object_clear_scale(Object *ob, const bool clear_delta)
{
  /* clear scale factors which are not locked */
  if ((ob->protectflag & OB_LOCK_SCALEX) == 0) {
    ob->scale[0] = 1.0f;
    if (clear_delta) {
      ob->dscale[0] = 1.0f;
    }
  }
  if ((ob->protectflag & OB_LOCK_SCALEY) == 0) {
    ob->scale[1] = 1.0f;
    if (clear_delta) {
      ob->dscale[1] = 1.0f;
    }
  }
  if ((ob->protectflag & OB_LOCK_SCALEZ) == 0) {
    ob->scale[2] = 1.0f;
    if (clear_delta) {
      ob->dscale[2] = 1.0f;
    }
  }
}

/* generic exec for clear-transform operators */
static wmOperatorStatus object_clear_transform_generic_exec(bContext *C,
                                                            wmOperator *op,
                                                            void (*clear_func)(Object *,
                                                                               const bool),
                                                            const char default_ksName[])
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  /* May be null. */
  View3D *v3d = CTX_wm_view3d(C);
  KeyingSet *ks;
  const bool clear_delta = RNA_boolean_get(op->ptr, "clear_delta");

  BLI_assert(!ELEM(nullptr, clear_func, default_ksName));

  Vector<Object *> objects;
  FOREACH_SELECTED_EDITABLE_OBJECT_BEGIN (view_layer, v3d, ob) {
    objects.append(ob);
  }
  FOREACH_SELECTED_EDITABLE_OBJECT_END;

  if (objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  /* Support transforming the object data. */
  const bool use_transform_skip_children = (scene->toolsettings->transform_flag &
                                            SCE_XFORM_SKIP_CHILDREN);
  const bool use_transform_data_origin = (scene->toolsettings->transform_flag &
                                          SCE_XFORM_DATA_ORIGIN);
  XFormObjectSkipChild_Container *xcs = nullptr;
  XFormObjectData_Container *xds = nullptr;

  if (use_transform_skip_children) {
    BKE_scene_graph_evaluated_ensure(depsgraph, bmain);
    xcs = xform_skip_child_container_create();
    xform_skip_child_container_item_ensure_from_array(
        xcs, scene, view_layer, objects.data(), objects.size());
  }
  if (use_transform_data_origin) {
    BKE_scene_graph_evaluated_ensure(depsgraph, bmain);
    xds = data_xform_container_create();
  }

  /* get KeyingSet to use */
  ks = animrig::get_keyingset_for_autokeying(scene, default_ksName);

  if (animrig::is_autokey_on(scene)) {
    ANIM_deselect_keys_in_animation_editors(C);
  }

  for (Object *ob : objects) {
    if (use_transform_data_origin) {
      data_xform_container_item_ensure(xds, ob);
    }

    /* run provided clearing function */
    clear_func(ob, clear_delta);

    animrig::autokeyframe_object(C, scene, ob, ks);

    /* tag for updates */
    DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
  }

  if (use_transform_skip_children) {
    object_xform_skip_child_container_update_all(xcs, bmain, depsgraph);
    object_xform_skip_child_container_destroy(xcs);
  }

  if (use_transform_data_origin) {
    data_xform_container_update_all(xds, bmain, depsgraph);
    data_xform_container_destroy(xds);
  }

  /* this is needed so children are also updated */
  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);

  return OPERATOR_FINISHED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clear Location Operator
 * \{ */

static wmOperatorStatus object_location_clear_exec(bContext *C, wmOperator *op)
{
  return object_clear_transform_generic_exec(C, op, object_clear_loc, ANIM_KS_LOCATION_ID);
}

void OBJECT_OT_location_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Location";
  ot->description = "Clear the object's location";
  ot->idname = "OBJECT_OT_location_clear";

  /* API callbacks. */
  ot->exec = object_location_clear_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_boolean(
      ot->srna,
      "clear_delta",
      false,
      "Clear Delta",
      "Clear delta location in addition to clearing the normal location transform");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clear Rotation Operator
 * \{ */

static wmOperatorStatus object_rotation_clear_exec(bContext *C, wmOperator *op)
{
  return object_clear_transform_generic_exec(C, op, object_clear_rot, ANIM_KS_ROTATION_ID);
}

void OBJECT_OT_rotation_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Rotation";
  ot->description = "Clear the object's rotation";
  ot->idname = "OBJECT_OT_rotation_clear";

  /* API callbacks. */
  ot->exec = object_rotation_clear_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_boolean(
      ot->srna,
      "clear_delta",
      false,
      "Clear Delta",
      "Clear delta rotation in addition to clearing the normal rotation transform");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clear Scale Operator
 * \{ */

static wmOperatorStatus object_scale_clear_exec(bContext *C, wmOperator *op)
{
  return object_clear_transform_generic_exec(C, op, object_clear_scale, ANIM_KS_SCALING_ID);
}

void OBJECT_OT_scale_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Scale";
  ot->description = "Clear the object's scale";
  ot->idname = "OBJECT_OT_scale_clear";

  /* API callbacks. */
  ot->exec = object_scale_clear_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  /* properties */
  ot->prop = RNA_def_boolean(
      ot->srna,
      "clear_delta",
      false,
      "Clear Delta",
      "Clear delta scale in addition to clearing the normal scale transform");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Clear Origin Operator
 * \{ */

static wmOperatorStatus object_origin_clear_exec(bContext *C, wmOperator * /*op*/)
{
  float *v1, *v3;
  float mat[3][3];

  CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
    if (ob->parent) {
      /* vectors pointed to by v1 and v3 will get modified */
      v1 = ob->loc;
      v3 = ob->parentinv[3];

      copy_m3_m4(mat, ob->parentinv);
      negate_v3_v3(v3, v1);
      mul_m3_v3(mat, v3);
    }

    DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
  }
  CTX_DATA_END;

  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_origin_clear(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Clear Origin";
  ot->description = "Clear the object's origin";
  ot->idname = "OBJECT_OT_origin_clear";

  /* API callbacks. */
  ot->exec = object_origin_clear_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply Transformation Operator
 * \{ */

/* use this when the loc/size/rot of the parent has changed but the children
 * should stay in the same place, e.g. for apply-size-rot or object center */
static void ignore_parent_tx(Main *bmain, Depsgraph *depsgraph, Scene *scene, Object *ob)
{
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);

  /* a change was made, adjust the children to compensate */
  for (Object &ob_child : bmain->objects) {
    if (ob_child.parent == ob) {
      Object *ob_child_eval = DEG_get_evaluated(depsgraph, &ob_child);
      BKE_object_apply_mat4(ob_child_eval, ob_child_eval->object_to_world().ptr(), true, false);
      invert_m4_m4(ob_child.parentinv,
                   BKE_object_calc_parent(depsgraph, scene, ob_child_eval).ptr());
      /* Copy result of BKE_object_apply_mat4(). */
      BKE_object_transform_copy(&ob_child, ob_child_eval);
      /* Make sure evaluated object is in a consistent state with the original one.
       * It might be needed for applying transform on its children. */
      copy_m4_m4(ob_child_eval->parentinv, ob_child.parentinv);
      BKE_object_eval_transform_all(depsgraph, scene_eval, ob_child_eval);
      /* Tag for update.
       * This is because parent matrix did change, so in theory the child object might now be
       * evaluated to a different location in another editing context. */
      DEG_id_tag_update(&ob_child.id, ID_RECALC_TRANSFORM);
    }
  }
}

static void append_sorted_object_parent_hierarchy(Object *root_object,
                                                  Object *object,
                                                  Object **sorted_objects,
                                                  int *object_index)
{
  if (!ELEM(object->parent, nullptr, root_object)) {
    append_sorted_object_parent_hierarchy(
        root_object, object->parent, sorted_objects, object_index);
  }
  if (object->id.tag & ID_TAG_DOIT) {
    sorted_objects[*object_index] = object;
    (*object_index)++;
    object->id.tag &= ~ID_TAG_DOIT;
  }
}

static Array<Object *> sorted_selected_editable_objects(bContext *C)
{
  Main *bmain = CTX_data_main(C);

  /* Count all objects, but also tag all the selected ones. */
  BKE_main_id_tag_all(bmain, ID_TAG_DOIT, false);
  int objects_num = 0;
  CTX_DATA_BEGIN (C, Object *, object, selected_editable_objects) {
    object->id.tag |= ID_TAG_DOIT;
    objects_num++;
  }
  CTX_DATA_END;
  if (objects_num == 0) {
    return {};
  }

  /* Append all the objects. */
  Array<Object *> sorted_objects(objects_num);
  int object_index = 0;
  CTX_DATA_BEGIN (C, Object *, object, selected_editable_objects) {
    if ((object->id.tag & ID_TAG_DOIT) == 0) {
      continue;
    }
    append_sorted_object_parent_hierarchy(object, object, sorted_objects.data(), &object_index);
  }
  CTX_DATA_END;

  return sorted_objects;
}

/**
 * Check if we need and can handle the special multiuser case.
 */
static bool apply_objects_internal_can_multiuser(bContext *C)
{
  Object *obact = CTX_data_active_object(C);

  if (ELEM(nullptr, obact, obact->data)) {
    return false;
  }

  if (ID_REAL_USERS(obact->data) == 1) {
    return false;
  }

  bool all_objects_same_data = true;
  bool obact_selected = false;

  CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
    if (ob->data != obact->data) {
      all_objects_same_data = false;
      break;
    }

    if (ob == obact) {
      obact_selected = true;
    }
  }
  CTX_DATA_END;

  return all_objects_same_data && obact_selected;
}

/**
 * Check if the current selection need to be made into single user.
 *
 * It assumes that all selected objects share the same object data.
 */
static bool apply_objects_internal_need_single_user(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  BLI_assert(apply_objects_internal_can_multiuser(C));

  /* Counting the number of objects is valid since it's known the
   * selection is only made up of users of the active objects data. */
  return (ID_REAL_USERS(ob->data) > CTX_DATA_COUNT(C, selected_editable_objects));
}

static wmOperatorStatus apply_objects_internal(bContext *C,
                                               ReportList *reports,
                                               bool apply_loc,
                                               bool apply_rot,
                                               bool apply_scale,
                                               bool do_props,
                                               bool do_single_user,
                                               bool corrective_flip_normals)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  float rsmat[3][3], obmat[3][3], iobmat[3][3], mat[4][4], scale;
  bool changed = true;
  bool const do_multi_user = apply_objects_internal_can_multiuser(C);
  float obact_invmat[4][4], obact_parent[4][4], obact_parentinv[4][4];

  /* Only used when do_multi_user is set. */
  Object *obact = nullptr;
  bool make_single_user = false;

  if (do_multi_user) {
    obact = CTX_data_active_object(C);
    invert_m4_m4(obact_invmat, obact->object_to_world().ptr());

    copy_m4_m4(obact_parent, BKE_object_calc_parent(depsgraph, scene, obact).ptr());
    copy_m4_m4(obact_parentinv, obact->parentinv);

    if (apply_objects_internal_need_single_user(C)) {
      if (do_single_user) {
        make_single_user = true;
      }
      else {
        ID *obact_data = obact->data;
        BKE_reportf(reports,
                    RPT_ERROR,
                    R"(Cannot apply to a multi user: Object "%s", %s "%s", aborting)",
                    obact->id.name + 2,
                    BKE_idtype_idcode_to_name(GS(obact_data->name)),
                    obact_data->name + 2);
        return OPERATOR_CANCELLED;
      }
    }
  }

  /* first check if we can execute */
  CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
    if (ELEM(ob->type,
             OB_MESH,
             OB_ARMATURE,
             OB_LATTICE,
             OB_MBALL,
             OB_CURVES_LEGACY,
             OB_SURF,
             OB_FONT,
             OB_CURVES,
             OB_POINTCLOUD,
             OB_GREASE_PENCIL))
    {
      ID *obdata = ob->data;
      if (!do_multi_user && ID_REAL_USERS(obdata) > 1) {
        BKE_reportf(reports,
                    RPT_ERROR,
                    R"(Cannot apply to a multi user: Object "%s", %s "%s", aborting)",
                    ob->id.name + 2,
                    BKE_idtype_idcode_to_name(GS(obdata->name)),
                    obdata->name + 2);
        changed = false;
      }

      if (!ID_IS_EDITABLE(obdata) || ID_IS_OVERRIDE_LIBRARY(obdata)) {
        BKE_reportf(reports,
                    RPT_ERROR,
                    R"(Cannot apply to library or override data: Object "%s", %s "%s", aborting)",
                    ob->id.name + 2,
                    BKE_idtype_idcode_to_name(GS(obdata->name)),
                    obdata->name + 2);
        changed = false;
      }
    }

    if (ELEM(ob->type, OB_CURVES_LEGACY, OB_SURF)) {
      ID *obdata = ob->data;
      Curve *cu = id_cast<Curve *>(ob->data);

      if (((ob->type == OB_CURVES_LEGACY) && !(cu->flag & CU_3D)) && (apply_rot || apply_loc)) {
        BKE_reportf(
            reports,
            RPT_ERROR,
            R"(Rotation/Location cannot apply to a 2D curve: Object "%s", %s "%s", aborting)",
            ob->id.name + 2,
            BKE_idtype_idcode_to_name(GS(obdata->name)),
            obdata->name + 2);
        changed = false;
      }
      if (cu->key) {
        BKE_reportf(reports,
                    RPT_ERROR,
                    R"(Can't apply to a curve with shape-keys: Object "%s", %s "%s", aborting)",
                    ob->id.name + 2,
                    BKE_idtype_idcode_to_name(GS(obdata->name)),
                    obdata->name + 2);
        changed = false;
      }
    }

    if (ob->type == OB_FONT) {
      if (apply_rot || apply_loc) {
        BKE_reportf(reports,
                    RPT_ERROR,
                    "Text objects can only have their scale applied: \"%s\"",
                    ob->id.name + 2);
        changed = false;
      }
    }

    if (ob->type == OB_LAMP) {
      Light *la = id_cast<Light *>(ob->data);
      if (la->type == LA_AREA) {
        if (apply_rot || apply_loc) {
          BKE_reportf(reports,
                      RPT_ERROR,
                      "Area Lights can only have scale applied: \"%s\"",
                      ob->id.name + 2);
          changed = false;
        }
      }
    }
  }
  CTX_DATA_END;

  if (!changed) {
    return OPERATOR_CANCELLED;
  }

  changed = false;

  /* now execute */

  if (make_single_user) {
    /* Make single user. */
    single_obdata_user_make(bmain, scene, obact);
    BKE_main_id_newptr_and_tag_clear(bmain);
    WM_event_add_notifier(C, NC_WINDOW, nullptr);
    DEG_relations_tag_update(bmain);
  }

  Array<Object *> objects = sorted_selected_editable_objects(C);
  if (objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  bool has_non_invertable_matrix = false;

  for (Object *ob : objects) {
    /* calculate rotation/scale matrix */
    if (apply_scale && apply_rot) {
      BKE_object_to_mat3(ob, rsmat);
    }
    else if (apply_scale) {
      BKE_object_scale_to_mat3(ob, rsmat);
    }
    else if (apply_rot) {
      float tmat[3][3], timat[3][3];

      /* simple rotation matrix */
      BKE_object_rot_to_mat3(ob, rsmat, true);

      /* correct for scale, note mul_m3_m3m3 has swapped args! */
      BKE_object_scale_to_mat3(ob, tmat);
      if (!invert_m3_m3(timat, tmat)) {
        BKE_reportf(
            reports,
            RPT_WARNING,
            "Object \"%s\" has a non-invertible transformation matrix, not applying transform",
            ob->id.name + 2);
        has_non_invertable_matrix = true;
        continue;
      }
      mul_m3_m3m3(rsmat, timat, rsmat);
      mul_m3_m3m3(rsmat, rsmat, tmat);
    }
    else {
      unit_m3(rsmat);
    }

    copy_m4_m3(mat, rsmat);

    /* calculate translation */
    if (apply_loc) {
      add_v3_v3v3(mat[3], ob->loc, ob->dloc);

      if (!(apply_scale && apply_rot)) {
        float tmat[3][3];
        /* correct for scale and rotation that is still applied */
        BKE_object_to_mat3(ob, obmat);
        invert_m3_m3(iobmat, obmat);
        mul_m3_m3m3(tmat, rsmat, iobmat);
        mul_m3_v3(tmat, mat[3]);
      }
    }

    /* apply to object data */
    if (do_multi_user && ob != obact) {
      /* Don't apply, just set the new object data, the correct
       * transformations will happen later. */
      id_us_min(ob->data);
      ob->data = obact->data;
      id_us_plus(ob->data);
    }
    else if (ob->type == OB_MESH) {
      Mesh *mesh = id_cast<Mesh *>(ob->data);

      if (apply_scale) {
        multiresModifier_scale_disp(depsgraph, scene, ob);
      }

      /* adjust data */
      bke::mesh_transform(*mesh, float4x4(mat), true);
      /* The determinant of mat will be negative for objects with an odd number of negative scale
       * axes, since the normals will be flipped when scale is applied, we provide the option to
       * flip them back. */
      if (corrective_flip_normals && math::determinant(float4x4(mat)) < 0.0f) {
        bke::mesh_flip_faces(*mesh, IndexMask(mesh->faces_num));
      }
    }
    else if (ob->type == OB_ARMATURE) {
      bArmature *arm = id_cast<bArmature *>(ob->data);
      BKE_armature_transform(arm, mat, do_props);
    }
    else if (ob->type == OB_LATTICE) {
      Lattice *lt = id_cast<Lattice *>(ob->data);

      BKE_lattice_transform(lt, mat, true);
    }
    else if (ob->type == OB_MBALL) {
      MetaBall *mb = id_cast<MetaBall *>(ob->data);
      BKE_mball_transform(mb, mat, do_props);
    }
    else if (ELEM(ob->type, OB_CURVES_LEGACY, OB_SURF)) {
      Curve *cu = id_cast<Curve *>(ob->data);
      scale = mat3_to_scale(rsmat);
      BKE_curve_transform_ex(cu, mat, true, do_props, scale);
    }
    else if (ob->type == OB_FONT) {
      Curve *cu = id_cast<Curve *>(ob->data);

      scale = mat3_to_scale(rsmat);

      for (int i = 0; i < cu->totbox; i++) {
        TextBox *tb = &cu->tb[i];
        tb->x *= scale;
        tb->y *= scale;
        tb->w *= scale;
        tb->h *= scale;
      }

      if (do_props) {
        cu->fsize *= scale;
      }
    }
    else if (ob->type == OB_CURVES) {
      Curves &curves = *id_cast<Curves *>(ob->data);
      curves.geometry.wrap().transform(float4x4(mat));
      curves.geometry.wrap().calculate_bezier_auto_handles();
    }
    else if (ob->type == OB_GREASE_PENCIL) {
      GreasePencil &grease_pencil = *id_cast<GreasePencil *>(ob->data);

      const float scalef = mat4_to_scale(mat);

      for (const int layer_i : grease_pencil.layers().index_range()) {
        bke::greasepencil::Layer &layer = grease_pencil.layer(layer_i);
        const float4x4 layer_to_object = layer.to_object_space(*ob);
        const float4x4 object_to_layer = math::invert(layer_to_object);
        const Map<bke::greasepencil::FramesMapKeyT, GreasePencilFrame> frames = layer.frames();
        frames.foreach_item(
            [&](bke::greasepencil::FramesMapKeyT /*key*/, GreasePencilFrame frame) {
              GreasePencilDrawingBase *base = grease_pencil.drawing(frame.drawing_index);
              if (base->type != GP_DRAWING) {
                return;
              }
              bke::greasepencil::Drawing &drawing =
                  reinterpret_cast<GreasePencilDrawing *>(base)->wrap();
              bke::CurvesGeometry &curves = drawing.strokes_for_write();
              MutableSpan<float> radii = drawing.radii_for_write();
              threading::parallel_for(radii.index_range(), 8192, [&](const IndexRange range) {
                for (const int i : range) {
                  radii[i] *= scalef;
                }
              });

              curves.transform(object_to_layer * float4x4(mat) * layer_to_object);
              curves.calculate_bezier_auto_handles();
            });
      }
    }
    else if (ob->type == OB_POINTCLOUD) {
      PointCloud &pointcloud = *id_cast<PointCloud *>(ob->data);
      math::transform_points(float4x4(mat), pointcloud.positions_for_write());
      pointcloud.tag_positions_changed();
    }
    else if (ob->type == OB_CAMERA) {
      MovieClip *clip = BKE_object_movieclip_get(scene, ob, false);

      /* applying scale on camera actually scales clip's reconstruction.
       * of there's clip assigned to camera nothing to do actually.
       */
      if (!clip) {
        continue;
      }

      if (apply_scale) {
        BKE_tracking_reconstruction_scale(&clip->tracking, ob->scale);
      }
    }
    else if (ob->type == OB_EMPTY) {
      /* It's possible for empties too, even though they don't
       * really have obdata, since we can simply apply the maximum
       * scaling to the empty's drawsize.
       *
       * Core Assumptions:
       * 1) Most scaled empties have uniform scaling
       *    (i.e. for visibility reasons), AND/OR
       * 2) Preserving non-uniform scaling is not that important,
       *    and is something that many users would be willing to
       *    sacrifice for having an easy way to do this.
       */

      if (apply_scale) {
        float max_scale = max_fff(fabsf(ob->scale[0]), fabsf(ob->scale[1]), fabsf(ob->scale[2]));
        ob->empty_drawsize *= max_scale;
      }
    }
    else if (ob->type == OB_LAMP) {
      Light *la = id_cast<Light *>(ob->data);
      if (la->type != LA_AREA) {
        continue;
      }

      bool keeps_aspect_ratio = compare_ff_relative(rsmat[0][0], rsmat[1][1], FLT_EPSILON, 64);
      if ((la->area_shape == LA_AREA_SQUARE) && !keeps_aspect_ratio) {
        la->area_shape = LA_AREA_RECT;
        la->area_sizey = la->area_size;
      }
      else if ((la->area_shape == LA_AREA_DISK) && !keeps_aspect_ratio) {
        la->area_shape = LA_AREA_ELLIPSE;
        la->area_sizey = la->area_size;
      }

      la->area_size *= rsmat[0][0];
      la->area_sizey *= rsmat[1][1];
      la->area_sizez *= rsmat[2][2];

      /* Explicit tagging is required for Lamp ID because, unlike Geometry IDs like Mesh,
       * it is not covered by the `ID_RECALC_GEOMETRY` flag applied to the object at the end
       * of this loop. */
      DEG_id_tag_update(&la->id, ID_RECALC_PARAMETERS);
    }
    else {
      continue;
    }

    if (do_multi_user && ob != obact) {
      float _obmat[4][4], _iobmat[4][4];
      float _mat[4][4];

      copy_m4_m4(_obmat, ob->object_to_world().ptr());
      invert_m4_m4(_iobmat, _obmat);

      copy_m4_m4(_mat, _obmat);
      mul_m4_m4_post(_mat, obact_invmat);
      mul_m4_m4_post(_mat, obact_parent);
      mul_m4_m4_post(_mat, obact_parentinv);

      if (apply_loc && apply_scale && apply_rot) {
        BKE_object_apply_mat4(ob, _mat, false, true);
      }
      else {
        Object ob_temp = dna::shallow_copy(*ob);
        BKE_object_apply_mat4(&ob_temp, _mat, false, true);

        if (apply_loc) {
          copy_v3_v3(ob->loc, ob_temp.loc);
        }

        if (apply_scale) {
          copy_v3_v3(ob->scale, ob_temp.scale);
        }

        if (apply_rot) {
          copy_v4_v4(ob->quat, ob_temp.quat);
          copy_v3_v3(ob->rot, ob_temp.rot);
          copy_v3_v3(ob->rotAxis, ob_temp.rotAxis);
          ob->rotAngle = ob_temp.rotAngle;
        }
      }
    }
    else {
      if (apply_loc) {
        zero_v3(ob->loc);
        zero_v3(ob->dloc);
      }
      if (apply_scale) {
        copy_v3_fl(ob->scale, 1.0f);
        copy_v3_fl(ob->dscale, 1.0f);
      }
      if (apply_rot) {
        zero_v3(ob->rot);
        zero_v3(ob->drot);
        unit_qt(ob->quat);
        unit_qt(ob->dquat);
        unit_axis_angle(ob->rotAxis, &ob->rotAngle);
        unit_axis_angle(ob->drotAxis, &ob->drotAngle);
      }
    }

    Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
    BKE_object_transform_copy(ob_eval, ob);

    BKE_object_where_is_calc(depsgraph, scene, ob_eval);
    if (ob->type == OB_ARMATURE) {
      /* needed for bone parents */
      BKE_armature_copy_bone_transforms(id_cast<bArmature *>(ob_eval->data),
                                        id_cast<bArmature *>(ob->data));
      BKE_pose_where_is(depsgraph, scene, ob_eval);
    }

    ignore_parent_tx(bmain, depsgraph, scene, ob);

    DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);

    changed = true;
  }

  if (!changed) {
    BKE_report(reports, RPT_WARNING, "Objects have no data to transform");
    return OPERATOR_CANCELLED;
  }
  if (has_non_invertable_matrix) {
    BKE_report(reports, RPT_WARNING, "Failed to apply rotation to some of the objects");
  }

  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);
  return OPERATOR_FINISHED;
}

static wmOperatorStatus visual_transform_apply_exec(bContext *C, wmOperator * /*op*/)
{
  Scene *scene = CTX_data_scene(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  bool changed = false;

  CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
    Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
    BKE_object_where_is_calc(depsgraph, scene, ob_eval);
    BKE_object_apply_mat4(ob_eval, ob_eval->object_to_world().ptr(), true, true);
    BKE_object_transform_copy(ob, ob_eval);

    /* update for any children that may get moved */
    DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);

    changed = true;
  }
  CTX_DATA_END;

  if (!changed) {
    return OPERATOR_CANCELLED;
  }

  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);
  return OPERATOR_FINISHED;
}

void OBJECT_OT_visual_transform_apply(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Apply Visual Transform";
  ot->description = "Apply the object's visual transformation to its data";
  ot->idname = "OBJECT_OT_visual_transform_apply";

  /* API callbacks. */
  ot->exec = visual_transform_apply_exec;
  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

static wmOperatorStatus object_transform_apply_exec(bContext *C, wmOperator *op)
{
  const bool loc = RNA_boolean_get(op->ptr, "location");
  const bool rot = RNA_boolean_get(op->ptr, "rotation");
  const bool sca = RNA_boolean_get(op->ptr, "scale");
  const bool do_props = RNA_boolean_get(op->ptr, "properties");
  const bool do_single_user = RNA_boolean_get(op->ptr, "isolate_users");
  const bool corrective_flip_normals = RNA_boolean_get(op->ptr, "corrective_flip_normals");

  if (loc || rot || sca) {
    return apply_objects_internal(
        C, op->reports, loc, rot, sca, do_props, do_single_user, corrective_flip_normals);
  }
  /* allow for redo */
  return OPERATOR_FINISHED;
}

static wmOperatorStatus object_transform_apply_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent * /*event*/)
{
  Object *ob = context_active_object(C);

  bool can_handle_multiuser = apply_objects_internal_can_multiuser(C);
  bool need_single_user = can_handle_multiuser && apply_objects_internal_need_single_user(C);

  if ((ob != nullptr) && (ob->data != nullptr) && need_single_user) {
    PropertyRNA *prop = RNA_struct_find_property(op->ptr, "isolate_users");
    if (!RNA_property_is_set(op->ptr, prop)) {
      RNA_property_boolean_set(op->ptr, prop, true);
    }
    if (RNA_property_boolean_get(op->ptr, prop)) {
      return WM_operator_confirm_ex(C,
                                    op,
                                    IFACE_("Apply Object Transformations"),
                                    IFACE_("Warning: Multiple objects share the same data.\nMake "
                                           "single user and then apply transformations?"),
                                    IFACE_("Apply"),
                                    ui::AlertIcon::Warning,
                                    false);
    }
  }
  return object_transform_apply_exec(C, op);
}

void OBJECT_OT_transform_apply(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Apply Object Transform";
  ot->description = "Apply the object's transformation to its data";
  ot->idname = "OBJECT_OT_transform_apply";

  /* API callbacks. */
  ot->exec = object_transform_apply_exec;
  ot->invoke = object_transform_apply_invoke;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(ot->srna, "location", true, "Location", "");
  RNA_def_boolean(ot->srna, "rotation", true, "Rotation", "");
  RNA_def_boolean(ot->srna, "scale", true, "Scale", "");
  RNA_def_boolean(ot->srna,
                  "properties",
                  true,
                  "Apply Properties",
                  "Modify properties such as curve vertex radius, font size and bone envelope");
  RNA_def_boolean(ot->srna,
                  "corrective_flip_normals",
                  true,
                  "Corrective Flip Normals",
                  "Invert normals for negative scaled objects.");
  PropertyRNA *prop = RNA_def_boolean(ot->srna,
                                      "isolate_users",
                                      false,
                                      "Isolate Multi User Data",
                                      "Create new object-data users if needed");
  RNA_def_property_flag(prop, PROP_HIDDEN);
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Apply Parent Inverse Operator
 * \{ */

static wmOperatorStatus object_parent_inverse_apply_exec(bContext *C, wmOperator * /*op*/)
{
  CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
    if (ob->parent == nullptr) {
      continue;
    }

    DEG_id_tag_update(&ob->id, ID_RECALC_TRANSFORM);
    BKE_object_apply_parent_inverse(ob);
  }
  CTX_DATA_END;

  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);

  return OPERATOR_FINISHED;
}

void OBJECT_OT_parent_inverse_apply(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Apply Parent Inverse";
  ot->description = "Apply the object's parent inverse to its data";
  ot->idname = "OBJECT_OT_parent_inverse_apply";

  /* API callbacks. */
  ot->exec = object_parent_inverse_apply_exec;
  ot->poll = ED_operator_objectmode;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Object Center Operator
 * \{ */

enum {
  GEOMETRY_TO_ORIGIN = 0,
  ORIGIN_TO_GEOMETRY,
  ORIGIN_TO_CURSOR,
  ORIGIN_TO_CENTER_OF_MASS_SURFACE,
  ORIGIN_TO_CENTER_OF_MASS_VOLUME,
};

static float3 arithmetic_mean(const Span<float3> values)
{
  if (values.is_empty()) {
    return float3(0);
  }
  /* TODO: Use a method that avoids overflow. */
  return std::accumulate(values.begin(), values.end(), float3(0)) / values.size();
}

static void translate_positions(MutableSpan<float3> positions, const float3 &translation)
{
  threading::parallel_for(positions.index_range(), 2048, [&](const IndexRange range) {
    for (float3 &position : positions.slice(range)) {
      position += translation;
    }
  });
}

static wmOperatorStatus object_origin_set_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Object *obact = CTX_data_active_object(C);
  Object *obedit = CTX_data_edit_object(C);
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  float3 cent, cent_neg, centn;
  const float *cursor = scene->cursor.location;
  int centermode = RNA_enum_get(op->ptr, "type");

  /* keep track of what is changed */
  int tot_change = 0, tot_lib_error = 0, tot_multiuser_arm_error = 0;

  if (obedit && centermode != GEOMETRY_TO_ORIGIN) {
    BKE_report(op->reports, RPT_ERROR, "Operation cannot be performed in edit mode");
    return OPERATOR_CANCELLED;
  }

  int around;
  {
    PropertyRNA *prop_center = RNA_struct_find_property(op->ptr, "center");
    if (RNA_property_is_set(op->ptr, prop_center)) {
      around = RNA_property_enum_get(op->ptr, prop_center);
    }
    else {
      if (scene->toolsettings->transform_pivot_point == V3D_AROUND_CENTER_BOUNDS) {
        around = V3D_AROUND_CENTER_BOUNDS;
      }
      else {
        around = V3D_AROUND_CENTER_MEDIAN;
      }
      RNA_property_enum_set(op->ptr, prop_center, around);
    }
  }

  zero_v3(cent);

  if (obedit) {
    if (obedit->type == OB_MESH) {
      Mesh *mesh = id_cast<Mesh *>(obedit->data);
      BMEditMesh *em = mesh->runtime->edit_mesh.get();
      BMVert *eve;
      BMIter iter;

      if (centermode == ORIGIN_TO_CURSOR) {
        copy_v3_v3(cent, cursor);
        invert_m4_m4(obedit->runtime->world_to_object.ptr(), obedit->object_to_world().ptr());
        mul_m4_v3(obedit->world_to_object().ptr(), cent);
      }
      else {
        if (around == V3D_AROUND_CENTER_BOUNDS) {
          float min[3], max[3];
          INIT_MINMAX(min, max);
          BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
            minmax_v3v3_v3(min, max, eve->co);
          }
          mid_v3_v3v3(cent, min, max);
        }
        else { /* #V3D_AROUND_CENTER_MEDIAN. */
          if (em->bm->totvert) {
            const float total_div = 1.0f / float(em->bm->totvert);
            BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
              madd_v3_v3fl(cent, eve->co, total_div);
            }
          }
        }
      }

      BM_ITER_MESH (eve, &iter, em->bm, BM_VERTS_OF_MESH) {
        sub_v3_v3(eve->co, cent);
      }

      EDBM_mesh_normals_update(em);
      tot_change++;
      DEG_id_tag_update(&obedit->id, ID_RECALC_GEOMETRY);
    }
  }

  Array<Object *> objects = sorted_selected_editable_objects(C);
  if (objects.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  /* reset flags */
  for (const int object_index : objects.index_range()) {
    Object *ob = objects[object_index];
    ob->flag &= ~OB_DONE;

    /* move active first */
    if (ob == obact && objects.size() > 1) {
      memmove(&objects[1], objects.data(), object_index * sizeof(Object *));
      objects[0] = ob;
    }
  }

  for (Object &tob : bmain->objects) {
    if (tob.data) {
      (tob.data)->tag &= ~ID_TAG_DOIT;
    }
    if (tob.instance_collection) {
      (id_cast<ID *>(tob.instance_collection))->tag &= ~ID_TAG_DOIT;
    }
  }

  bool reported_empty = false;
  std::array<bool, INDEX_ID_MAX> reported{false};
  for (Object *ob : objects) {
    if (ob->flag & OB_DONE) {
      continue;
    }

    bool do_inverse_offset = false;
    ob->flag |= OB_DONE;

    if (centermode == ORIGIN_TO_CURSOR) {
      copy_v3_v3(cent, cursor);
      invert_m4_m4(ob->runtime->world_to_object.ptr(), ob->object_to_world().ptr());
      mul_m4_v3(ob->world_to_object().ptr(), cent);
    }

    if (ob->data == nullptr) {
      /* Special support for instanced collections. */
      if ((ob->transflag & OB_DUPLICOLLECTION) && ob->instance_collection &&
          (ob->instance_collection->id.tag & ID_TAG_DOIT) == 0)
      {
        if (!BKE_id_is_editable(bmain, &ob->instance_collection->id)) {
          tot_lib_error++;
        }
        else {
          if (centermode == ORIGIN_TO_CURSOR) {
            /* done */
          }
          else {
            float3 min, max;
            /* only bounds support */
            INIT_MINMAX(min, max);
            BKE_object_minmax_dupli(depsgraph, ob, min, max, true);
            mid_v3_v3v3(cent, min, max);
            invert_m4_m4(ob->runtime->world_to_object.ptr(), ob->object_to_world().ptr());
            mul_m4_v3(ob->world_to_object().ptr(), cent);
          }
          add_v3_v3(ob->instance_collection->instance_offset, cent);

          tot_change++;
          ob->instance_collection->id.tag |= ID_TAG_DOIT;
          do_inverse_offset = true;
        }
      }
      else {
        BLI_assert(ob->type == OB_EMPTY);
        if (!reported_empty) {
          reported_empty = true;
          BKE_report(op->reports, RPT_INFO, "Set Origin not supported for Empty object(s)");
        }
      }
    }
    else if (!ID_IS_EDITABLE(ob->data) || ID_IS_OVERRIDE_LIBRARY(ob->data)) {
      tot_lib_error++;
    }
    else if (ob->type == OB_MESH) {
      if (obedit == nullptr) {
        Mesh *mesh = id_cast<Mesh *>(ob->data);

        if (centermode == ORIGIN_TO_CURSOR) {
          /* done */
        }
        else if (centermode == ORIGIN_TO_CENTER_OF_MASS_SURFACE) {
          BKE_mesh_center_of_surface(mesh, cent);
        }
        else if (centermode == ORIGIN_TO_CENTER_OF_MASS_VOLUME) {
          BKE_mesh_center_of_volume(mesh, cent);
        }
        else if (around == V3D_AROUND_CENTER_BOUNDS) {
          if (const std::optional<Bounds<float3>> bounds = mesh->bounds_min_max()) {
            cent = math::midpoint(bounds->min, bounds->max);
          }
        }
        else { /* #V3D_AROUND_CENTER_MEDIAN. */
          BKE_mesh_center_median(mesh, cent);
        }

        negate_v3_v3(cent_neg, cent);
        bke::mesh_translate(*mesh, cent_neg, true);

        tot_change++;
        mesh->id.tag |= ID_TAG_DOIT;
        do_inverse_offset = true;
      }
    }
    else if (ELEM(ob->type, OB_CURVES_LEGACY, OB_SURF)) {
      Curve *cu = id_cast<Curve *>(ob->data);

      if (centermode == ORIGIN_TO_CURSOR) {
        /* done */
      }
      else if (around == V3D_AROUND_CENTER_BOUNDS) {
        if (std::optional<Bounds<float3>> bounds = BKE_curve_minmax(cu, true)) {
          cent = math::midpoint(bounds->min, bounds->max);
        }
      }
      else { /* #V3D_AROUND_CENTER_MEDIAN. */
        BKE_curve_center_median(cu, cent);
      }

      /* don't allow Z change if curve is 2D */
      if ((ob->type == OB_CURVES_LEGACY) && !(cu->flag & CU_3D)) {
        cent[2] = 0.0;
      }

      negate_v3_v3(cent_neg, cent);
      BKE_curve_translate(cu, cent_neg, true);

      tot_change++;
      cu->id.tag |= ID_TAG_DOIT;
      do_inverse_offset = true;

      if (obedit) {
        if (centermode == GEOMETRY_TO_ORIGIN) {
          DEG_id_tag_update(&obedit->id, ID_RECALC_GEOMETRY);
        }
        break;
      }
    }
    else if (ob->type == OB_FONT) {
      /* Get from bounding-box. */

      Curve *cu = id_cast<Curve *>(ob->data);
      std::optional<Bounds<float3>> bounds = BKE_curve_minmax(cu, true);

      if (!bounds && (centermode != ORIGIN_TO_CURSOR)) {
        /* Do nothing. */
      }
      else {
        if (centermode == ORIGIN_TO_CURSOR) {
          /* Done. */
        }
        else {
          /* extra 0.5 is the height o above line */
          cent = math::midpoint(bounds->min, bounds->max);
        }

        cent[2] = 0.0f;

        cu->xof = cu->xof - cent[0];
        cu->yof = cu->yof - cent[1];

        tot_change++;
        cu->id.tag |= ID_TAG_DOIT;
        do_inverse_offset = true;
      }
    }
    else if (ob->type == OB_ARMATURE) {
      bArmature *arm = id_cast<bArmature *>(ob->data);

      if (ID_REAL_USERS(arm) > 1) {
#if 0
        BKE_report(op->reports, RPT_ERROR, "Cannot apply to a multi user armature");
        return;
#endif
        tot_multiuser_arm_error++;
      }
      else {
        /* Function to recenter armatures in editarmature.c
         * Bone + object locations are handled there.
         */
        ED_armature_origin_set(bmain, ob, cursor, centermode, around);

        tot_change++;
        arm->id.tag |= ID_TAG_DOIT;
        // do_inverse_offset = true; /* docenter_armature() handles this. */

        Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
        BKE_object_transform_copy(ob_eval, ob);
        BKE_armature_copy_bone_transforms(id_cast<bArmature *>(ob_eval->data),
                                          id_cast<bArmature *>(ob->data));
        BKE_object_where_is_calc(depsgraph, scene, ob_eval);
        BKE_pose_where_is(depsgraph, scene, ob_eval); /* needed for bone parents */

        ignore_parent_tx(bmain, depsgraph, scene, ob);

        if (obedit) {
          break;
        }
      }
    }
    else if (ob->type == OB_MBALL) {
      MetaBall *mb = id_cast<MetaBall *>(ob->data);

      if (centermode == ORIGIN_TO_CURSOR) {
        /* done */
      }
      else if (around == V3D_AROUND_CENTER_BOUNDS) {
        BKE_mball_center_bounds(mb, cent);
      }
      else { /* #V3D_AROUND_CENTER_MEDIAN. */
        BKE_mball_center_median(mb, cent);
      }

      negate_v3_v3(cent_neg, cent);
      BKE_mball_translate(mb, cent_neg);

      tot_change++;
      mb->id.tag |= ID_TAG_DOIT;
      do_inverse_offset = true;

      if (obedit) {
        if (centermode == GEOMETRY_TO_ORIGIN) {
          DEG_id_tag_update(&obedit->id, ID_RECALC_GEOMETRY);
        }
        break;
      }
    }
    else if (ob->type == OB_LATTICE) {
      Lattice *lt = id_cast<Lattice *>(ob->data);

      if (centermode == ORIGIN_TO_CURSOR) {
        /* done */
      }
      else if (around == V3D_AROUND_CENTER_BOUNDS) {
        if (std::optional<Bounds<float3>> bounds = BKE_lattice_minmax(lt)) {
          cent = math::midpoint(bounds->min, bounds->max);
        }
      }
      else { /* #V3D_AROUND_CENTER_MEDIAN. */
        BKE_lattice_center_median(lt, cent);
      }

      negate_v3_v3(cent_neg, cent);
      BKE_lattice_translate(lt, cent_neg, true);

      tot_change++;
      lt->id.tag |= ID_TAG_DOIT;
      do_inverse_offset = true;
    }
    else if (ob->type == OB_CURVES) {
      Curves &curves_id = *id_cast<Curves *>(ob->data);
      bke::CurvesGeometry &curves = curves_id.geometry.wrap();
      if (ELEM(centermode, ORIGIN_TO_CENTER_OF_MASS_SURFACE, ORIGIN_TO_CENTER_OF_MASS_VOLUME) ||
          !ELEM(around, V3D_AROUND_CENTER_BOUNDS, V3D_AROUND_CENTER_MEDIAN))
      {
        BKE_report(
            op->reports, RPT_WARNING, "Curves Object does not support this set origin operation");
        continue;
      }

      if (curves.is_empty()) {
        continue;
      }

      if (centermode == ORIGIN_TO_CURSOR) {
        /* done */
      }
      else if (around == V3D_AROUND_CENTER_BOUNDS) {
        const Bounds<float3> bounds = *curves.bounds_min_max();
        cent = math::midpoint(bounds.min, bounds.max);
      }
      else if (around == V3D_AROUND_CENTER_MEDIAN) {
        cent = arithmetic_mean(curves.positions());
      }

      tot_change++;
      curves.translate(-cent);
      curves_id.id.tag |= ID_TAG_DOIT;
      do_inverse_offset = true;
    }
    else if (ob->type == OB_GREASE_PENCIL) {
      GreasePencil &grease_pencil = *id_cast<GreasePencil *>(ob->data);
      if (ELEM(centermode, ORIGIN_TO_CENTER_OF_MASS_SURFACE, ORIGIN_TO_CENTER_OF_MASS_VOLUME) ||
          !ELEM(around, V3D_AROUND_CENTER_BOUNDS, V3D_AROUND_CENTER_MEDIAN))
      {
        BKE_report(op->reports,
                   RPT_WARNING,
                   "Grease Pencil Object does not support this set origin operation");
        continue;
      }

      if (centermode == ORIGIN_TO_CURSOR) {
        /* done */
      }
      else if (around == V3D_AROUND_CENTER_BOUNDS) {
        const int current_frame = scene->r.cfra;
        const Bounds<float3> bounds = *grease_pencil.bounds_min_max(current_frame);
        cent = math::midpoint(bounds.min, bounds.max);
      }
      else if (around == V3D_AROUND_CENTER_MEDIAN) {
        const int current_frame = scene->r.cfra;
        float3 center = float3(0.0f);
        int total_points = 0;

        for (const int layer_i : grease_pencil.layers().index_range()) {
          const bke::greasepencil::Layer &layer = grease_pencil.layer(layer_i);
          const float4x4 layer_to_object = layer.local_transform();
          if (!layer.is_visible()) {
            continue;
          }
          if (const bke::greasepencil::Drawing *drawing = grease_pencil.get_drawing_at(
                  layer, current_frame))
          {
            const bke::CurvesGeometry &curves = drawing->strokes();
            const Span<float3> positions = curves.positions();

            for (const int i : positions.index_range()) {
              center += math::transform_point(layer_to_object, positions[i]);
            }
            total_points += positions.size();
          }
        }

        if (total_points != 0) {
          cent = center / total_points;
        }
      }

      tot_change++;

      for (const int layer_i : grease_pencil.layers().index_range()) {
        bke::greasepencil::Layer &layer = grease_pencil.layer(layer_i);
        const float4x4 layer_to_object = layer.local_transform();
        const float4x4 object_to_layer = math::invert(layer_to_object);
        const Map<bke::greasepencil::FramesMapKeyT, GreasePencilFrame> frames = layer.frames();
        frames.foreach_item(
            [&](bke::greasepencil::FramesMapKeyT /*key*/, GreasePencilFrame frame) {
              GreasePencilDrawingBase *base = grease_pencil.drawing(frame.drawing_index);
              if (base->type != GP_DRAWING) {
                return;
              }
              bke::greasepencil::Drawing &drawing =
                  reinterpret_cast<GreasePencilDrawing *>(base)->wrap();
              bke::CurvesGeometry &curves = drawing.strokes_for_write();

              curves.translate(math::transform_direction(object_to_layer, -cent));
              curves.calculate_bezier_auto_handles();
            });
      }

      grease_pencil.id.tag |= ID_TAG_DOIT;
      do_inverse_offset = true;
    }
    else if (ob->type == OB_POINTCLOUD) {
      PointCloud &pointcloud = *id_cast<PointCloud *>(ob->data);
      MutableSpan<float3> positions = pointcloud.positions_for_write();
      if (ELEM(centermode, ORIGIN_TO_CENTER_OF_MASS_SURFACE, ORIGIN_TO_CENTER_OF_MASS_VOLUME) ||
          !ELEM(around, V3D_AROUND_CENTER_BOUNDS, V3D_AROUND_CENTER_MEDIAN))
      {
        BKE_report(op->reports,
                   RPT_WARNING,
                   "Point cloud object does not support this set origin operation");
        continue;
      }

      if (centermode == ORIGIN_TO_CURSOR) {
        /* Done. */
      }
      else if (around == V3D_AROUND_CENTER_BOUNDS) {
        if (const std::optional<Bounds<float3>> bounds = pointcloud.bounds_min_max()) {
          cent = math::midpoint(bounds->min, bounds->max);
        }
      }
      else if (around == V3D_AROUND_CENTER_MEDIAN) {
        cent = arithmetic_mean(positions);
      }

      tot_change++;
      translate_positions(positions, -cent);
      pointcloud.tag_positions_changed();
      pointcloud.id.tag |= ID_TAG_DOIT;
      do_inverse_offset = true;
    }
    else {
      const ID *obdata = static_cast<const ID *>(ob->data);
      const short idcode = GS(obdata->name);
      const int id_index = BKE_idtype_idcode_to_index(idcode);

      if (!reported[id_index]) {
        reported[id_index] = true;
        BKE_reportf(op->reports,
                    RPT_INFO,
                    "Set Origin not supported for %s object(s)",
                    BKE_idtype_idcode_to_name(idcode));
      }
    }

    /* offset other selected objects */
    if (do_inverse_offset && (centermode != GEOMETRY_TO_ORIGIN)) {
      float obmat[4][4];

      /* was the object data modified
       * NOTE: the functions above must set 'cent'. */

      /* convert the offset to parent space */
      BKE_object_to_mat4(ob, obmat);
      mul_v3_mat3_m4v3(centn, obmat, cent); /* omit translation part */

      add_v3_v3(ob->loc, centn);

      Object *ob_eval = DEG_get_evaluated(depsgraph, ob);
      BKE_object_transform_copy(ob_eval, ob);
      BKE_object_where_is_calc(depsgraph, scene, ob_eval);
      if (ob->type == OB_ARMATURE) {
        /* needed for bone parents */
        BKE_armature_copy_bone_transforms(id_cast<bArmature *>(ob_eval->data),
                                          id_cast<bArmature *>(ob->data));
        BKE_pose_where_is(depsgraph, scene, ob_eval);
      }

      ignore_parent_tx(bmain, depsgraph, scene, ob);

      /* other users? */
      // CTX_DATA_BEGIN (C, Object *, ob_other, selected_editable_objects)
      //{

      /* use existing context looper */
      for (Object *ob_other : objects) {
        if ((ob_other->flag & OB_DONE) == 0 &&
            ((ob->data && (ob->data == ob_other->data)) ||
             (ob->instance_collection == ob_other->instance_collection &&
              (ob->transflag | ob_other->transflag) & OB_DUPLICOLLECTION)))
        {
          ob_other->flag |= OB_DONE;
          DEG_id_tag_update(&ob_other->id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);

          mul_v3_mat3_m4v3(
              centn, ob_other->object_to_world().ptr(), cent); /* omit translation part */
          add_v3_v3(ob_other->loc, centn);

          Object *ob_other_eval = DEG_get_evaluated(depsgraph, ob_other);
          BKE_object_transform_copy(ob_other_eval, ob_other);
          BKE_object_where_is_calc(depsgraph, scene, ob_other_eval);
          if (ob_other->type == OB_ARMATURE) {
            /* needed for bone parents */
            BKE_armature_copy_bone_transforms(id_cast<bArmature *>(ob_eval->data),
                                              id_cast<bArmature *>(ob->data));
            BKE_pose_where_is(depsgraph, scene, ob_other_eval);
          }
          ignore_parent_tx(bmain, depsgraph, scene, ob_other);
        }
      }
      // CTX_DATA_END;
    }
  }

  for (Object &tob : bmain->objects) {
    if (tob.data && ((tob.data)->tag & ID_TAG_DOIT)) {
      BKE_object_batch_cache_dirty_tag(&tob);
      DEG_id_tag_update(&tob.id, ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY);
    }
    /* Special support for dupli-groups. */
    else if (tob.instance_collection && tob.instance_collection->id.tag & ID_TAG_DOIT) {
      DEG_id_tag_update(&tob.id, ID_RECALC_TRANSFORM);
      DEG_id_tag_update(&tob.instance_collection->id, ID_RECALC_SYNC_TO_EVAL);
    }
  }

  if (tot_change) {
    WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, nullptr);
  }

  /* Warn if any errors occurred */
  if (tot_lib_error + tot_multiuser_arm_error) {
    BKE_reportf(op->reports,
                RPT_WARNING,
                "%i object(s) not centered, %i changed:",
                tot_lib_error + tot_multiuser_arm_error,
                tot_change);
    if (tot_lib_error) {
      BKE_reportf(op->reports, RPT_WARNING, "|%i linked library object(s)", tot_lib_error);
    }
    if (tot_multiuser_arm_error) {
      BKE_reportf(
          op->reports, RPT_WARNING, "|%i multiuser armature object(s)", tot_multiuser_arm_error);
    }
  }

  return OPERATOR_FINISHED;
}

void OBJECT_OT_origin_set(wmOperatorType *ot)
{
  static const EnumPropertyItem prop_set_center_types[] = {
      {GEOMETRY_TO_ORIGIN,
       "GEOMETRY_ORIGIN",
       0,
       "Geometry to Origin",
       "Move object geometry to object origin"},
      {ORIGIN_TO_GEOMETRY,
       "ORIGIN_GEOMETRY",
       0,
       "Origin to Geometry",
       "Calculate the center of geometry based on the current pivot point (median, otherwise "
       "bounding box)"},
      {ORIGIN_TO_CURSOR,
       "ORIGIN_CURSOR",
       0,
       "Origin to 3D Cursor",
       "Move object origin to position of the 3D cursor"},
      /* Intentional naming mismatch since some scripts refer to this. */
      {ORIGIN_TO_CENTER_OF_MASS_SURFACE,
       "ORIGIN_CENTER_OF_MASS",
       0,
       "Origin to Center of Mass (Surface)",
       "Calculate the center of mass from the surface area"},
      {ORIGIN_TO_CENTER_OF_MASS_VOLUME,
       "ORIGIN_CENTER_OF_VOLUME",
       0,
       "Origin to Center of Mass (Volume)",
       "Calculate the center of mass from the volume (must be manifold geometry with consistent "
       "normals)"},
      {0, nullptr, 0, nullptr, nullptr},
  };

  static const EnumPropertyItem prop_set_bounds_types[] = {
      {V3D_AROUND_CENTER_MEDIAN, "MEDIAN", 0, "Median Center", ""},
      {V3D_AROUND_CENTER_BOUNDS, "BOUNDS", 0, "Bounds Center", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  /* identifiers */
  ot->name = "Set Origin";
  ot->description =
      "Set the object's origin, by either moving the data, or set to center of data, or use 3D "
      "cursor";
  ot->idname = "OBJECT_OT_origin_set";

  /* API callbacks. */
  ot->invoke = WM_menu_invoke;
  ot->exec = object_origin_set_exec;

  ot->poll = ED_operator_scene_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  ot->prop = RNA_def_enum(ot->srna, "type", prop_set_center_types, 0, "Type", "");
  RNA_def_enum(ot->srna, "center", prop_set_bounds_types, V3D_AROUND_CENTER_MEDIAN, "Center", "");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Axis Target
 *
 * Note this is an experimental operator to point lights/cameras at objects.
 * We may re-work how this behaves based on user feedback.
 * - campbell.
 * \{ */

/** When using multiple objects, apply their relative rotational offset to the active object. */
#define USE_RELATIVE_ROTATION
/** Disable overlays, ignoring user setting (light wire gets in the way). */
#define USE_RENDER_OVERRIDE
/**
 * Calculate a depth if the cursor isn't already over a depth
 * (not essential but feels buggy without).
 */
#define USE_FAKE_DEPTH_INIT

/* -------------------------------------------------------------------- */
/** \name Normals Modal Keymap
 * \{ */

enum {
  TGT_MODAL_CONFIRM = 1,
  TGT_MODAL_CANCEL,
  TGT_MODAL_DIFFUSE_ENABLE,
  TGT_MODAL_DIFFUSE_DISABLE,
  TGT_MODAL_SPECULAR_ENABLE,
  TGT_MODAL_SPECULAR_DISABLE,
  TGT_MODAL_SHADOW_ENABLE,
  TGT_MODAL_SHADOW_DISABLE,
  TGT_MODAL_SWITCH_TO_ORBIT,
  TGT_MODAL_PRECISION_ENABLE,
  TGT_MODAL_PRECISION_DISABLE,
};

void object_target_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {TGT_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},
      {TGT_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {TGT_MODAL_DIFFUSE_ENABLE,
       "DIFFUSE_ENABLE",
       0,
       "Diffuse Mode",
       "Position light depending on the object's normals"},
      {TGT_MODAL_DIFFUSE_DISABLE, "DIFFUSE_DISABLE", 0, "Diffuse Mode (Off)", ""},
      {TGT_MODAL_SPECULAR_ENABLE,
       "SPECULAR_ENABLE",
       0,
       "Specular Mode",
       "Position light depending on the specular reflection"},
      {TGT_MODAL_SPECULAR_DISABLE, "SPECULAR_DISABLE", 0, "Specular Mode (Off)", ""},
      {TGT_MODAL_SHADOW_ENABLE,
       "SHADOW_ENABLE",
       0,
       "Shadow Mode",
       "Position light depending on the shadow target"},
      {TGT_MODAL_SHADOW_DISABLE, "SHADOW_DISABLE", 0, "Shadow Mode (Off)", ""},
      {TGT_MODAL_SWITCH_TO_ORBIT,
       "SWITCH_TO_ORBIT",
       0,
       "Switch to Orbit",
       "Switch to orbit mode around target"},
      {TGT_MODAL_PRECISION_ENABLE,
       "PRECISION_ENABLE",
       0,
       "Precision Mode",
       "Position lights more precisly"},
      {TGT_MODAL_PRECISION_DISABLE, "PRECISION_DISABLE", 0, "Precision Mode (Off)", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Object Target Modal Map");

  /* This function is called for each space-type, only needs to add map once. */
  if (keymap && keymap->modal_items) {
    return;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "Object Target Modal Map", modal_items);
  /* Assign map to operators. */
  WM_modalkeymap_assign(keymap, "OBJECT_OT_transform_axis_target");
}
/** \} */
enum LightPositioningMode {
  LIGHT_TARGET_MODE = 0,   /* Default: Target mode (rotation to cursor) */
  LIGHT_DIFFUSE_MODE = 1,  /* Position light along surface normal */
  LIGHT_SPECULAR_MODE = 2, /* Position light for specular reflection */
  LIGHT_SHADOW_MODE = 3,   /* Position light for shadow casting */
};

struct XFormAxisItem {
  Object *ob;
  float rot_mat[3][3];
  void *obtfm;
  float xform_dist;
  bool is_z_flip;

#ifdef USE_RELATIVE_ROTATION
  /* use when translating multiple */
  float xform_rot_offset[3][3];
#endif
};

struct XFormAxisData {
  ViewContext vc;
  ViewDepths *depths;
  struct {
    float depth;
    float3 normal;
    bool is_depth_valid;
    bool is_normal_valid;
  } prev;

  Vector<XFormAxisItem> object_data;

  int init_event;

  /* Light positioning data */
  LightPositioningMode light_mode;
  bool is_light_positioning;
  float light_offset_distance;
  float3 shadow_target_location;
  bool shadow_target_set;

  /* Navigation support */
  ViewOpsData *vod;
  bool run_navigation;

  /* Precision mode support */
  bool precision_mode;
  float precision_factor;
  int precision_toggle_mval[2]; /* Mouse position when precision mode was toggled */
};

#ifdef USE_FAKE_DEPTH_INIT
static void object_transform_axis_target_calc_depth_init(XFormAxisData *xfd, const int mval[2])
{
  float view_co_a[3], view_co_b[3];
  const float2 mval_fl = {float(mval[0]), float(mval[1])};
  ED_view3d_win_to_ray(xfd->vc.region, mval_fl, view_co_a, view_co_b);
  add_v3_v3(view_co_b, view_co_a);
  float center[3] = {0.0f};
  int center_tot = 0;
  for (XFormAxisItem &item : xfd->object_data) {
    const Object *ob = item.ob;
    const float *ob_co_a = ob->object_to_world().location();
    float ob_co_b[3];
    add_v3_v3v3(ob_co_b, ob->object_to_world().location(), ob->object_to_world().ptr()[2]);
    float view_isect[3], ob_isect[3];
    if (isect_line_line_v3(view_co_a, view_co_b, ob_co_a, ob_co_b, view_isect, ob_isect)) {
      add_v3_v3(center, view_isect);
      center_tot += 1;
    }
  }
  if (center_tot) {
    mul_v3_fl(center, 1.0f / center_tot);
    float center_proj[3];
    ED_view3d_project_v3(xfd->vc.region, center, center_proj);
    xfd->prev.depth = center_proj[2];
    xfd->prev.is_depth_valid = true;
  }
}
#endif /* USE_FAKE_DEPTH_INIT */

static bool object_is_target_compat(const Object *ob)
{
  if (ob->type == OB_LAMP) {
    const Light *la = id_cast<Light *>(ob->data);
    if (ELEM(la->type, LA_LOCAL, LA_SUN, LA_SPOT, LA_AREA)) {
      return true;
    }
  }
  else if (ob->type == OB_CAMERA) {
    return true;
  }
  return false;
}

static void object_transform_axis_target_free_data(bContext *C, wmOperator *op)
{
  XFormAxisData *xfd = static_cast<XFormAxisData *>(op->customdata);

#ifdef USE_RENDER_OVERRIDE
  if (xfd->depths) {
    ED_view3d_depths_free(xfd->depths);
  }
#endif

  /* Cleanup navigation data */
  if (xfd->vod) {
    ED_view3d_navigation_free(C, xfd->vod);
  }

  for (XFormAxisItem &item : xfd->object_data) {
    MEM_freeN(item.obtfm);
  }
  MEM_delete(xfd);
  op->customdata = nullptr;
}

/* We may want to expose as alternative to: BKE_object_apply_rotation */
static void object_apply_rotation(Object *ob, const float rmat[3][3])
{
  float size[3];
  float loc[3];
  float rmat4[4][4];
  copy_m4_m3(rmat4, rmat);
  copy_v3_v3(size, ob->scale);
  copy_v3_v3(loc, ob->loc);
  BKE_object_apply_mat4(ob, rmat4, true, true);
  copy_v3_v3(ob->scale, size);
  copy_v3_v3(ob->loc, loc);
}
/* We may want to extract this to: BKE_object_apply_location */
static void object_apply_location(Object *ob, const float loc[3])
{
  /* quick but weak */
  Object ob_prev = dna::shallow_copy(*ob);
  float mat[4][4];
  copy_m4_m4(mat, ob->object_to_world().ptr());
  copy_v3_v3(mat[3], loc);
  BKE_object_apply_mat4(ob, mat, true, true);
  copy_v3_v3(mat[3], ob->loc);
  *ob = dna::shallow_copy(ob_prev);
  copy_v3_v3(ob->loc, mat[3]);
}

static bool object_orient_to_location(Object *ob,
                                      const float rot_orig[3][3],
                                      const float axis[3],
                                      const float location[3],
                                      const bool z_flip)
{
  float delta[3];
  sub_v3_v3v3(delta, ob->object_to_world().location(), location);
  if (normalize_v3(delta) != 0.0f) {
    if (z_flip) {
      negate_v3(delta);
    }

    if (len_squared_v3v3(delta, axis) > FLT_EPSILON) {
      float delta_rot[3][3];
      float final_rot[3][3];
      rotation_between_vecs_to_mat3(delta_rot, axis, delta);

      mul_m3_m3m3(final_rot, delta_rot, rot_orig);

      object_apply_rotation(ob, final_rot);

      return true;
    }
  }
  return false;
}

static void object_transform_axis_target_cancel(bContext *C, wmOperator *op)
{
  XFormAxisData *xfd = static_cast<XFormAxisData *>(op->customdata);
  for (XFormAxisItem &item : xfd->object_data) {
    BKE_object_tfm_restore(item.ob, item.obtfm);
    DEG_id_tag_update(&item.ob->id, ID_RECALC_TRANSFORM);
    WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, item.ob);
  }

  /* Clear WorkspaceStatus to return to basic state */
  ED_workspace_status_text(C, nullptr);

  object_transform_axis_target_free_data(C, op);
}

static wmOperatorStatus object_transform_axis_target_invoke(bContext *C,
                                                            wmOperator *op,
                                                            const wmEvent *event)
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

  if (vc.obact == nullptr || !object_is_target_compat(vc.obact)) {
    /* Falls back to texture space transform. */
    return OPERATOR_PASS_THROUGH;
  }

#ifdef USE_RENDER_OVERRIDE
  int flag2_prev = vc.v3d->flag2;
  vc.v3d->flag2 |= V3D_HIDE_OVERLAYS;
#endif

  ViewDepths *depths = nullptr;
  ED_view3d_depth_override(
      vc.depsgraph, vc.region, vc.v3d, nullptr, V3D_DEPTH_NO_GPENCIL, false, &depths);

#ifdef USE_RENDER_OVERRIDE
  vc.v3d->flag2 = flag2_prev;
#endif

  if (depths == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "Unable to access depth buffer, using view plane");
    return OPERATOR_CANCELLED;
  }

  ED_region_tag_redraw(vc.region);

  XFormAxisData *xfd = MEM_new<XFormAxisData>(__func__);
  op->customdata = xfd;

  /* Don't change this at runtime. */
  xfd->vc = vc;
  xfd->depths = depths;
  xfd->vc.mval[0] = event->mval[0];
  xfd->vc.mval[1] = event->mval[1];

  xfd->prev.depth = 1.0f;
  xfd->prev.is_depth_valid = false;
  xfd->prev.is_normal_valid = false;

  xfd->init_event = WM_userdef_event_type_from_keymap_type(event->type);

  /* Initialize light positioning */
  xfd->light_mode = LIGHT_TARGET_MODE;
  xfd->is_light_positioning = object_is_target_compat(xfd->vc.obact);
  zero_v3(xfd->shadow_target_location);
  xfd->shadow_target_set = false;

  /* Initialize navigation support */
  xfd->vod = ED_view3d_navigation_init(C, nullptr);
  xfd->run_navigation = false;

  /* Initialize precision mode */
  xfd->precision_mode = false;
  xfd->precision_factor = 0.1f;

  /* Calculate initial offset distance from light to geometry intersection */
  float calculated_distance = 0.0f;
  bool distance_calculated = false;

  if (xfd->is_light_positioning && object_is_target_compat(xfd->vc.obact)) {
    /* Cast ray from light along its local Z-axis to find geometry intersection */
    Object *light = xfd->vc.obact;

    /* Get light's normal direction (local Z-axis in world space) */
    float3 light_normal;
    copy_v3_v3(light_normal, light->object_to_world().ptr()[2]);
    negate_v3(light_normal); /* Light points in negative Z direction by default */

    /* Use snap system to cast ray from light position */
    blender::ed::transform::SnapObjectParams snap_params = {};
    snap_params.snap_target_select = SCE_SNAP_TARGET_ALL;
    snap_params.edit_mode_type = blender::ed::transform::SNAP_GEOM_FINAL;
    snap_params.occlusion_test = blender::ed::transform::SNAP_OCCLUSION_NEVER;
    snap_params.use_backface_culling = false;
    snap_params.keep_on_same_target = false;
    snap_params.face_nearest_steps = 1;
    snap_params.grid_size = 0.0f;

    blender::ed::transform::SnapObjectContext *sctx =
        blender::ed::transform::snap_object_context_create();

    float3 hit_co, hit_no;
    float ray_depth = BVH_RAYCAST_DIST_MAX; /* Cast ray far into the scene */

    bool hit = blender::ed::transform::snap_object_project_ray(
        sctx,
        xfd->vc.depsgraph,
        xfd->vc.v3d,
        &snap_params,
        light->object_to_world().location(), /* ray start from light position */
        light_normal,                        /* ray direction along light normal */
        &ray_depth,
        hit_co,
        hit_no);

    blender::ed::transform::snap_object_context_destroy(sctx);

    if (hit) {
      /* Calculate distance from light to geometry intersection */
      calculated_distance = len_v3v3(light->object_to_world().location(), hit_co);
      if (calculated_distance >= 0.1f) {
        xfd->light_offset_distance = calculated_distance;
        distance_calculated = true;
      }
    }
  }

  /* If we couldn't calculate the real distance, use simple fallback */
  if (!distance_calculated) {
    xfd->light_offset_distance = 5.0f; /* Simple fallback for all cases (Arbritrary value) */
  }

  xfd->object_data.append({});
  xfd->object_data.last().ob = xfd->vc.obact;

  CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
    if ((ob != xfd->vc.obact) && object_is_target_compat(ob)) {
      xfd->object_data.append({});
      xfd->object_data.last().ob = ob;
    }
  }
  CTX_DATA_END;

  for (XFormAxisItem &item : xfd->object_data) {
    item.obtfm = BKE_object_tfm_backup(item.ob);
    BKE_object_rot_to_mat3(item.ob, item.rot_mat, true);

    /* Detect negative scale matrix. */
    float full_mat3[3][3];
    BKE_object_to_mat3(item.ob, full_mat3);
    item.is_z_flip = dot_v3v3(item.rot_mat[2], full_mat3[2]) < 0.0f;
  }

  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus object_transform_axis_target_modal(bContext *C,
                                                           wmOperator *op,
                                                           const wmEvent *event)
{
  XFormAxisData *xfd = static_cast<XFormAxisData *>(op->customdata);
  ARegion *region = xfd->vc.region;

  view3d_operator_needs_gpu(C);

  /* Handle navigation events */
  if (xfd->vod && ED_view3d_navigation_do(C, xfd->vod, event, nullptr)) {
    xfd->run_navigation = true;
    return OPERATOR_RUNNING_MODAL;
  }

  /* Handle modal keymap events for light positioning mode changes */
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case TGT_MODAL_CONFIRM: {
        Scene *scene = CTX_data_scene(C);
        /* Perform auto-keying for rotational changes for all objects. */
        for (XFormAxisItem &item : xfd->object_data) {
          autokeyframe_object_rotation(C, scene, item.ob);
        }
        /* Clear WorkspaceStatus to return to basic state */
        ED_workspace_status_text(C, nullptr);
        object_transform_axis_target_free_data(C, op);
        return OPERATOR_FINISHED;
      }
      case TGT_MODAL_CANCEL: {
        object_transform_axis_target_cancel(C, op);
        return OPERATOR_CANCELLED;
      }
      case TGT_MODAL_DIFFUSE_ENABLE: {
        if (xfd->is_light_positioning) {
          xfd->light_mode = LIGHT_DIFFUSE_MODE;
        }
        break;
      }
      case TGT_MODAL_DIFFUSE_DISABLE: {
        if (xfd->is_light_positioning) {
          xfd->light_mode = LIGHT_TARGET_MODE;
        }
        break;
      }
      case TGT_MODAL_SPECULAR_ENABLE: {
        if (xfd->is_light_positioning) {
          xfd->light_mode = LIGHT_SPECULAR_MODE;
        }
        break;
      }
      case TGT_MODAL_SPECULAR_DISABLE: {
        if (xfd->is_light_positioning) {
          xfd->light_mode = LIGHT_TARGET_MODE;
        }
        break;
      }
      case TGT_MODAL_SHADOW_ENABLE: {
        if (xfd->is_light_positioning) {
          /* Handle shadow mode activation - set target when entering shadow mode */
          if (!xfd->shadow_target_set) {
            /* Place shadow target at current mouse position */
            const ViewDepths *depths = xfd->depths;
            if (depths && (event->mval[0] < depths->w) && (event->mval[1] < depths->h)) {
              float depth_fl = 1.0f;
              ED_view3d_depth_read_cached(depths, event->mval, 0, &depth_fl);
              if (xfd->prev.is_depth_valid && depth_fl == 1.0f) {
                depth_fl = xfd->prev.depth;
              }
              const double depth = double(depth_fl);
              if ((depth > depths->depth_range[0]) && (depth < depths->depth_range[1])) {
                float3 target_location;
                if (ED_view3d_depth_unproject_v3(
                        xfd->vc.region, event->mval, depth, target_location))
                {
                  copy_v3_v3(xfd->shadow_target_location, target_location);
                  xfd->shadow_target_set = true;
                }
              }
            }
          }
          xfd->light_mode = LIGHT_SHADOW_MODE;
        }
        break;
      }
      case TGT_MODAL_SHADOW_DISABLE: {
        if (xfd->is_light_positioning) {
          xfd->light_mode = LIGHT_TARGET_MODE;
          /* Reset shadow target when exiting shadow mode */
          xfd->shadow_target_set = false;
        }
        break;
      }
      case TGT_MODAL_SWITCH_TO_ORBIT: {
        object_transform_axis_target_free_data(C, op);
        WM_operator_name_call(C,
                              "OBJECT_OT_orbit_around_target",
                              wm::OpCallContext::InvokeDefault,
                              nullptr,
                              nullptr);
        return OPERATOR_FINISHED;
      }
      case TGT_MODAL_PRECISION_ENABLE: {
        xfd->precision_mode = true;
        /* Store current mouse position as the base for precision calculations */
        xfd->precision_toggle_mval[0] = event->mval[0];
        xfd->precision_toggle_mval[1] = event->mval[1];
        break;
      }
      case TGT_MODAL_PRECISION_DISABLE: {
        xfd->precision_mode = false;
        break;
      }
      default:
        break;
    }
  }

  /* Display status bar for light positioning */
  if (xfd->is_light_positioning) {
    WorkspaceStatus status(C);
    status.opmodal(IFACE_("Confirm"), op->type, TGT_MODAL_CONFIRM);
    status.opmodal(IFACE_("Cancel"), op->type, TGT_MODAL_CANCEL);
    /* Show current mode and available mode switches */
    status.opmodal(IFACE_("Diffuse"),
                   op->type,
                   TGT_MODAL_DIFFUSE_ENABLE,
                   xfd->light_mode == LIGHT_DIFFUSE_MODE);
    status.opmodal(IFACE_("Specular"),
                   op->type,
                   TGT_MODAL_SPECULAR_ENABLE,
                   xfd->light_mode == LIGHT_SPECULAR_MODE);
    status.opmodal(
        IFACE_("Shadow"), op->type, TGT_MODAL_SHADOW_ENABLE, xfd->light_mode == LIGHT_SHADOW_MODE);
    status.opmodal(IFACE_("Orbit"), op->type, TGT_MODAL_SWITCH_TO_ORBIT);
    /* Show precision mode status */
    status.opmodal(IFACE_("Precision"), op->type, TGT_MODAL_PRECISION_ENABLE, xfd->precision_mode);
  }

  /* Refresh depth buffer after navigation */
  if (xfd->run_navigation) {
    xfd->run_navigation = false;

    /* Free old depth buffer */
    if (xfd->depths) {
      ED_view3d_depths_free(xfd->depths);
      xfd->depths = nullptr;
    }

    /* Create new depth buffer with updated view matrix */
#ifdef USE_RENDER_OVERRIDE
    int flag2_prev = xfd->vc.v3d->flag2;
    xfd->vc.v3d->flag2 |= V3D_HIDE_OVERLAYS;
#endif

    ViewDepths *depths = nullptr;
    ED_view3d_depth_override(xfd->vc.depsgraph,
                             xfd->vc.region,
                             xfd->vc.v3d,
                             nullptr,
                             V3D_DEPTH_NO_GPENCIL,
                             false,
                             &depths);

#ifdef USE_RENDER_OVERRIDE
    xfd->vc.v3d->flag2 = flag2_prev;
#endif

    if (depths != nullptr) {
      xfd->depths = depths;
      /* Clear cached depth and normal data to force recalculation */
      xfd->prev.is_depth_valid = false;
      xfd->prev.is_normal_valid = false;
    }
  }

  if (event->type == MOUSEMOVE) {
    const ViewDepths *depths = xfd->depths;

    /* Calculate effective mouse position with precision mode support */
    int effective_mval[2];
    precision_mode_mouse_pos(xfd->precision_mode,
                             xfd->precision_factor,
                             event->mval,
                             xfd->precision_toggle_mval,
                             effective_mval);

    if (depths && (uint(effective_mval[0]) < depths->w) && (uint(effective_mval[1]) < depths->h)) {
      float depth_fl = 1.0f;
      ED_view3d_depth_read_cached(depths, effective_mval, 0, &depth_fl);
      float3 location_world;
      if (depth_fl == 1.0f) {
        if (xfd->prev.is_depth_valid) {
          depth_fl = xfd->prev.depth;
        }
      }

#ifdef USE_FAKE_DEPTH_INIT
      /* First time only. */
      if (depth_fl == 1.0f) {
        if (xfd->prev.is_depth_valid == false) {
          object_transform_axis_target_calc_depth_init(xfd, event->mval);
          if (xfd->prev.is_depth_valid) {
            depth_fl = xfd->prev.depth;
          }
        }
      }
#endif

      double depth = double(depth_fl);
      if ((depth > depths->depth_range[0]) && (depth < depths->depth_range[1])) {
        xfd->prev.depth = depth_fl;
        xfd->prev.is_depth_valid = true;
        if (ED_view3d_depth_unproject_v3(region, effective_mval, depth, location_world)) {
          if (xfd->is_light_positioning && xfd->light_mode != LIGHT_TARGET_MODE) {

            float3 normal;
            bool normal_found = false;
            if (get_smoothed_surface_normal(&xfd->vc, depths, effective_mval, normal)) {
              normal_found = true;
            }
            else if (xfd->prev.is_normal_valid) {
              copy_v3_v3(normal, xfd->prev.normal);
              normal_found = true;
            }

            /* Handle special light positioning modes (reflection and shadow) */
            if (xfd->is_light_positioning && normal_found &&
                (xfd->light_mode == LIGHT_SPECULAR_MODE || xfd->light_mode == LIGHT_SHADOW_MODE))
            {
              for (XFormAxisItem &item : xfd->object_data) {
                if (!object_is_target_compat(item.ob)) {
                  continue; /* Skip non-light objects */
                }

                /* The offset distance is consistent throughout the modal execution.
                 * It's calculated once at initialization and only changes with Z-axis adjustment.
                 */

                float3 final_location;
                float3 final_normal;
                float3 view_dir;
                float3 reflected_dir;
                float3 direction_to_target;

                switch (xfd->light_mode) {
                  case LIGHT_TARGET_MODE:
                    /* Target mode: use original rotation behavior (no light positioning) */
                    /* This should not reach here as target mode uses original logic */
                  case LIGHT_DIFFUSE_MODE:
                    /* Normal mode: position light along surface normal */
                    position_light_along(normal,
                                         location_world,
                                         xfd->light_offset_distance,
                                         final_location,
                                         final_normal);
                    break;
                  case LIGHT_SPECULAR_MODE:
                    /* Reflection positioning: calculate reflection direction */
                    {
                      float2 mval = {float(event->mval[0]), float(event->mval[1])};
                      ED_view3d_win_to_vector(xfd->vc.region, mval, view_dir);
                      normalize_v3(view_dir);
                      /* Calculate reflection direction using Blender's reflect function */
                      reflect_v3_v3v3(reflected_dir, view_dir, normal);

                      position_light_along(reflected_dir,
                                           location_world,
                                           xfd->light_offset_distance,
                                           final_location,
                                           final_normal);
                    }
                    break;
                  case LIGHT_SHADOW_MODE:
                    /* Shadow positioning: position light to cast shadows from target */
                    if (xfd->shadow_target_set) {
                      sub_v3_v3v3(
                          direction_to_target, xfd->shadow_target_location, location_world);
                      normalize_v3(direction_to_target);

                      position_light_along(direction_to_target,
                                           xfd->shadow_target_location,
                                           xfd->light_offset_distance,
                                           final_location,
                                           final_normal);
                    }
                    else {
                      /* Fallback to normal mode if shadow target not set */
                      position_light_along(normal,
                                           location_world,
                                           xfd->light_offset_distance,
                                           final_location,
                                           final_normal);
                    }
                    break;
                  default:
                    /* Should not reach here since we filter the modes above */
                    position_light_along(normal,
                                         location_world,
                                         xfd->light_offset_distance,
                                         final_location,
                                         final_normal);
                    break;
                }

                /* Apply the position and orientation */
                object_apply_location(item.ob, final_location);
                copy_v3_v3(item.ob->runtime->object_to_world.location(), final_location);

                /* Orient light toward the target */
                float3 target_location;
                switch (xfd->light_mode) {
                  case LIGHT_SHADOW_MODE:
                    if (xfd->shadow_target_set) {
                      copy_v3_v3(target_location, xfd->shadow_target_location);
                    }
                    else {
                      copy_v3_v3(target_location, location_world);
                    }
                    break;
                  default:
                    copy_v3_v3(target_location, location_world);
                    break;
                }

                object_orient_to_location(
                    item.ob, item.rot_mat, item.rot_mat[2], target_location, item.is_z_flip);

                DEG_id_tag_update(&item.ob->id, ID_RECALC_TRANSFORM);
                WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, item.ob);
              }

              if (normal_found) {
                copy_v3_v3(xfd->prev.normal, normal);
                xfd->prev.is_normal_valid = true;
              }
            }
            else {
              /* Original non-light positioning logic */

              for (const int i : xfd->object_data.index_range()) {
                XFormAxisItem &item = xfd->object_data[i];
                /* For light positioning, use the fixed offset distance to maintain consistency */
                if (xfd->is_light_positioning && object_is_target_compat(item.ob)) {
                  item.xform_dist = xfd->light_offset_distance;
                }
                else {
                  float3 ob_axis;
                  item.xform_dist = len_v3v3(item.ob->object_to_world().location(),
                                             location_world);
                  normalize_v3_v3(ob_axis, item.ob->object_to_world().ptr()[2]);
                  /* Scale to avoid adding distance when moving between surfaces. */
                  if (normal_found) {
                    float scale = fabsf(dot_v3v3(ob_axis, normal));
                    item.xform_dist *= scale;
                  }
                }

                float3 target_normal;

                if (normal_found) {
                  copy_v3_v3(target_normal, normal);
                }
                else {
                  normalize_v3_v3(target_normal, item.ob->object_to_world().ptr()[2]);
                }

#ifdef USE_RELATIVE_ROTATION
                if (normal_found) {
                  if (i != 0) {
                    mul_m3_v3(item.xform_rot_offset, target_normal);
                  }
                }
#endif
                {
                  float3 loc;

                  copy_v3_v3(loc, location_world);
                  /* For light positioning, use the fixed offset distance to maintain consistency
                   */
                  float offset_distance = (xfd->is_light_positioning &&
                                           object_is_target_compat(item.ob)) ?
                                              xfd->light_offset_distance :
                                              item.xform_dist;
                  madd_v3_v3fl(loc, target_normal, offset_distance);
                  object_apply_location(item.ob, loc);
                  /* so orient behaves as expected */
                  copy_v3_v3(item.ob->runtime->object_to_world.location(), loc);
                }

                object_orient_to_location(
                    item.ob, item.rot_mat, item.rot_mat[2], location_world, item.is_z_flip);

                DEG_id_tag_update(&item.ob->id, ID_RECALC_TRANSFORM);
                WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, item.ob);
              }
              if (normal_found) {
                copy_v3_v3(xfd->prev.normal, normal);
                xfd->prev.is_normal_valid = true;
              }
            } /* End of original positioning logic */
          }
          else {
            for (XFormAxisItem &item : xfd->object_data) {
              if (object_orient_to_location(
                      item.ob, item.rot_mat, item.rot_mat[2], location_world, item.is_z_flip))
              {
                DEG_id_tag_update(&item.ob->id, ID_RECALC_TRANSFORM);
                WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, item.ob);
              }
            }
            xfd->prev.is_normal_valid = false;
          }
        }
      }
    }

    ED_region_tag_redraw(xfd->vc.region);
  }

  bool is_finished = false;

  if (ISMOUSE_BUTTON(xfd->init_event)) {
    if ((event->type == xfd->init_event) && (event->val == KM_RELEASE)) {
      is_finished = true;
    }
  }
  else {
    if (ELEM(event->type, LEFTMOUSE, EVT_RETKEY, EVT_PADENTER)) {
      is_finished = true;
    }
  }

  if (is_finished) {
    Scene *scene = CTX_data_scene(C);
    /* Perform auto-keying for rotational changes for all objects. */
    for (XFormAxisItem &item : xfd->object_data) {
      autokeyframe_object_rotation(C, scene, item.ob);
    }

    /* Clear WorkspaceStatus to return to basic state */
    ED_workspace_status_text(C, nullptr);

    object_transform_axis_target_free_data(C, op);
    return OPERATOR_FINISHED;
  }
  if (ELEM(event->type, EVT_ESCKEY, RIGHTMOUSE)) {
    object_transform_axis_target_cancel(C, op);
    return OPERATOR_CANCELLED;
  }

  /* Switch to orbit mode when 'O' is pressed. */
  if (event->type == EVT_OKEY && event->val == KM_PRESS) {
    /* Confirm current target operation. */
    Scene *scene = CTX_data_scene(C);
    for (XFormAxisItem &item : xfd->object_data) {
      autokeyframe_object_rotation(C, scene, item.ob);
    }
    object_transform_axis_target_free_data(C, op);

    /* Launch orbit operator. */
    WM_operator_name_call(
        C, "OBJECT_OT_orbit_around_target", wm::OpCallContext::InvokeDefault, nullptr, nullptr);
    return OPERATOR_FINISHED;
  }

  return OPERATOR_RUNNING_MODAL;
}

void OBJECT_OT_transform_axis_target(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Track to Cursor";
  ot->description =
      "Interactively point cameras and lights to a location. It can be used to point lights to "
      "object normals, specular reflections, or shadow targets";
  ot->idname = "OBJECT_OT_transform_axis_target";

  /* API callbacks. */
  ot->invoke = object_transform_axis_target_invoke;
  ot->cancel = object_transform_axis_target_cancel;
  ot->modal = object_transform_axis_target_modal;
  ot->poll = ED_operator_region_view3d_active;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

#undef USE_RELATIVE_ROTATION

/** \} */

/* -------------------------------------------------------------------- */
/** \name Object Orbit Around Target Modal Operator
 * \{ */

enum eOrbitAxisLock {
  ORB_AXIS_LOCK_NONE = 0,
  ORB_AXIS_LOCK_AZIMUTH = 1,
  ORB_AXIS_LOCK_ELEVATION = 2,
  ORB_AXIS_LOCK_DISTANCE = 3,
};

enum eObjectOrbitAroundTargetModal {
  ORB_MODAL_CONFIRM = 1,
  ORB_MODAL_CANCEL,
  ORB_MODAL_SWITCH_TO_TARGET,  /* T key - switch to target modal. */
  ORB_MODAL_AZIMUTH_LOCK,      /* H key - horizontal lock. */
  ORB_MODAL_ELEVATION_LOCK,    /* V key - vertical lock. */
  ORB_MODAL_DISTANCE_LOCK,     /* Z key - distance lock. */
  ORB_MODAL_INVERT,            /* I key - 180° rotation around local Y. */
  ORB_MODAL_SYMMETRY,          /* S key - symmetry around intersection point. */
  ORB_MODAL_PRECISION_ENABLE,  /* Left Shift - enable precision mode. */
  ORB_MODAL_PRECISION_DISABLE, /* Left Shift release - disable precision mode. */
};

struct ObjectOrbitAroundTargetData {
  ViewContext vc{};
  float2 init_mval{0.0f};
  float2 current_mval{0.0f}; /* Current mouse position when switching modes. */
  float3 center{0.0f};       /* Pivot point (hit point). */
  float distance = 0.0f;     /* Distance from light to pivot. */
  bool has_center = false;
  int init_event = 0;
  eOrbitAxisLock axis_lock = eOrbitAxisLock(0); /* Current axis constraint mode. */

  /* Precision mode support */
  bool precision_mode = false;
  float precision_factor = 0.1f;
  float2 precision_toggle_mval{0.0f}; /* Mouse position when precision mode was toggled */

  /* Navigation support */
  ViewOpsData *vod = nullptr;
  bool run_navigation = false;

  struct ObjectData {
    Object *ob = nullptr;
    float3 orig_loc{0.0f};
    /* Store original rotation data in all possible formats to avoid lossy conversions. */
    float3 orig_rot{0.0f};           /* Euler rotation. */
    float4 orig_quat{0.0f};          /* Quaternion rotation. */
    float3 orig_rot_axis{0.0f};      /* Axis-angle rotation axis. */
    float orig_rot_angle = 0.0f;     /* Axis-angle rotation angle. */
    float current_azimuth = 0.0f;    /* Current azimuth in spherical coordinates. */
    float current_elevation = 0.0f;  /* Current elevation in spherical coordinates. */
    float current_distance = 0.0f;   /* Current distance from center. */
    bool direction_inverted = false; /* Whether light direction is inverted. */
  };
  Vector<ObjectData> objects{};
};

static void object_orbit_around_target_set_cursor(bContext *C,
                                                  const ObjectOrbitAroundTargetData *lead)
{
  wmWindow *win = CTX_wm_window(C);

  switch (lead->axis_lock) {
    case ORB_AXIS_LOCK_AZIMUTH:
      WM_cursor_set(win, WM_CURSOR_EW_ARROW); /* East-West arrow for azimuth. */
      break;
    case ORB_AXIS_LOCK_ELEVATION:
      WM_cursor_set(win, WM_CURSOR_NS_ARROW); /* North-South arrow for elevation. */
      break;
    case ORB_AXIS_LOCK_DISTANCE:
      WM_cursor_set(win, WM_CURSOR_NS_ARROW); /* North-South arrow for distance. */
      break;
    case ORB_AXIS_LOCK_NONE:
    default:
      WM_cursor_set(win, WM_CURSOR_NSEW_SCROLL); /* All directions for free movement. */
      break;
  }
}

static void object_orbit_around_target_update_status(bContext *C,
                                                     wmOperator *op,
                                                     const ObjectOrbitAroundTargetData *lead)
{
  WorkspaceStatus status(C);
  status.opmodal(IFACE_("Cancel"), op->type, ORB_MODAL_CANCEL);
  status.opmodal(IFACE_("Confirm"), op->type, ORB_MODAL_CONFIRM);
  status.opmodal(IFACE_("Target Mode"), op->type, ORB_MODAL_SWITCH_TO_TARGET);
  status.opmodal(IFACE_("Horizontal Lock"),
                 op->type,
                 ORB_MODAL_AZIMUTH_LOCK,
                 lead->axis_lock == ORB_AXIS_LOCK_AZIMUTH);
  status.opmodal(IFACE_("Vertical Lock"),
                 op->type,
                 ORB_MODAL_ELEVATION_LOCK,
                 lead->axis_lock == ORB_AXIS_LOCK_ELEVATION);
  status.opmodal(IFACE_("Distance Lock"),
                 op->type,
                 ORB_MODAL_DISTANCE_LOCK,
                 lead->axis_lock == ORB_AXIS_LOCK_DISTANCE);

  /* Show direction inversion state for symmetry. */
  bool any_inverted = false;
  for (const ObjectOrbitAroundTargetData::ObjectData &light : lead->objects) {
    if (light.direction_inverted) {
      any_inverted = true;
      break;
    }
  }
  status.opmodal(IFACE_("Symmetry"), op->type, ORB_MODAL_SYMMETRY, any_inverted);
  status.opmodal(IFACE_("Invert Direction"), op->type, ORB_MODAL_INVERT);
  status.opmodal(
      IFACE_("Precision Mode"), op->type, ORB_MODAL_PRECISION_ENABLE, lead->precision_mode);
}

static void object_orbit_around_target_init_data(bContext *C, wmOperator *op, const wmEvent *event)
{
  ObjectOrbitAroundTargetData *ooatd = MEM_new<ObjectOrbitAroundTargetData>(__func__);
  op->customdata = ooatd;

  ooatd->vc = ED_view3d_viewcontext_init(C, CTX_data_depsgraph_pointer(C));
  ooatd->init_mval = float2(event->mval);
  ooatd->current_mval = ooatd->init_mval;
  ooatd->init_event = event->type;
  ooatd->has_center = false;
  ooatd->axis_lock = ORB_AXIS_LOCK_NONE;
  ooatd->vod = nullptr;
  ooatd->run_navigation = false;

  /* Set initial cursor. */
  object_orbit_around_target_set_cursor(C, ooatd);

  /* Set initial status text. */
  object_orbit_around_target_update_status(C, op, ooatd);

  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(scene, view_layer);

  Object *active_ob = ooatd->vc.obact;
  if (active_ob && ELEM(active_ob->type, OB_LAMP, OB_CAMERA)) {
    CTX_DATA_BEGIN (C, Object *, ob, selected_editable_objects) {
      if (!ELEM(ob->type, OB_LAMP, OB_CAMERA)) {
        continue;
      }

      ObjectOrbitAroundTargetData::ObjectData light_data;
      light_data.ob = ob;
      light_data.orig_loc = ob->loc;
      light_data.orig_rot = ob->rot;
      light_data.orig_quat = ob->quat;
      light_data.orig_rot_axis = ob->rotAxis;
      light_data.orig_rot_angle = ob->rotAngle;

      if (ob == active_ob) {
        ooatd->objects.prepend(light_data); /* Active light first */
      }
      else {
        ooatd->objects.append(light_data); /* Then the others */
      }
    }
    CTX_DATA_END;
  }

  /* Find pivot point by casting ray from light along its local Z-axis. */
  ooatd->has_center = false;

  if (!ooatd->objects.is_empty()) {
    /* Use the first selected light to determine the pivot point. */
    const Object *primary_light = ooatd->objects[0].ob;

    float3 light_normal;
    copy_v3_v3(light_normal, primary_light->object_to_world().ptr()[2]);
    negate_v3(light_normal);

    blender::ed::transform::SnapObjectParams snap_params = {};
    snap_params.snap_target_select = SCE_SNAP_TARGET_ALL;
    snap_params.edit_mode_type = blender::ed::transform::SNAP_GEOM_FINAL;
    snap_params.occlusion_test = blender::ed::transform::SNAP_OCCLUSION_NEVER;
    snap_params.use_backface_culling = false;
    snap_params.keep_on_same_target = false;
    snap_params.face_nearest_steps = 1;
    snap_params.grid_size = 0.0f;

    blender::ed::transform::SnapObjectContext *sctx =
        blender::ed::transform::snap_object_context_create();

    float3 hit_co, hit_no;
    float ray_depth = BVH_RAYCAST_DIST_MAX; /* Cast ray to maximum distance. */

    const bool hit = blender::ed::transform::snap_object_project_ray(
        sctx,
        ooatd->vc.depsgraph,
        ooatd->vc.v3d,
        &snap_params,
        primary_light->loc, /* ray start from light position. */
        light_normal,       /* ray direction along light normal. */
        &ray_depth,
        hit_co,
        hit_no);

    blender::ed::transform::snap_object_context_destroy(sctx);

    if (hit) {
      copy_v3_v3(ooatd->center, hit_co);
      ooatd->has_center = true;
      ooatd->distance = len_v3v3(primary_light->loc, hit_co);
    }
  }

  /* Fallback: Try cursor hit point if no light intersection found. */
  if (!ooatd->has_center) {
    float3 hit_co;
    if (ED_view3d_autodist(ooatd->vc.region, ooatd->vc.v3d, event->mval, hit_co, nullptr)) {
      copy_v3_v3(ooatd->center, hit_co);
      ooatd->has_center = true;

      /* Calculate average distance from lights to hit point. */
      if (!ooatd->objects.is_empty()) {
        float total_distance = 0.0f;
        for (const ObjectOrbitAroundTargetData::ObjectData &light : ooatd->objects) {
          total_distance += len_v3v3(light.ob->loc, hit_co);
        }
        ooatd->distance = total_distance / ooatd->objects.size();
      }
    }
  }

  /* Final fallback: use view center. */
  if (!ooatd->has_center) {
    copy_v3_v3(ooatd->center, ooatd->vc.rv3d->ofs);
    negate_v3(ooatd->center);
    ooatd->has_center = true;
    ooatd->distance = ooatd->vc.rv3d->dist;
  }

  /* Initialize current spherical coordinates for each light. */
  for (ObjectOrbitAroundTargetData::ObjectData &light : ooatd->objects) {
    /* Calculate direction from center to light position. */
    float3 dir;
    sub_v3_v3v3(dir, light.orig_loc, ooatd->center);
    light.current_distance = normalize_v3(dir);

    if (light.current_distance == 0.0f) {
      light.current_distance = ooatd->distance;
      light.current_azimuth = 0.0f;
      light.current_elevation = 0.0f;
    }
    else {
      /* Convert to spherical coordinates. */
      light.current_azimuth = atan2f(dir[0], dir[1]);
      light.current_elevation = safe_asinf(dir[2]);
    }
  }
}

/* Modal keymap for customizable keybindings. */
void object_orbit_around_target_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {ORB_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},
      {ORB_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {ORB_MODAL_SWITCH_TO_TARGET, "TARGET_MODE", 0, "Switch to Target mode", ""},
      {ORB_MODAL_AZIMUTH_LOCK, "AZIMUTH_LOCK", 0, "Horizontal Lock", ""},
      {ORB_MODAL_ELEVATION_LOCK, "ELEVATION_LOCK", 0, "Vertical Lock", ""},
      {ORB_MODAL_DISTANCE_LOCK, "DISTANCE_LOCK", 0, "Distance Lock", ""},
      {ORB_MODAL_INVERT, "INVERT", 0, "Invert Direction", ""},
      {ORB_MODAL_SYMMETRY, "SYMMETRY", 0, "Symmetry", ""},
      {ORB_MODAL_PRECISION_ENABLE, "PRECISION_ENABLE", 0, "Precision On", ""},
      {ORB_MODAL_PRECISION_DISABLE, "PRECISION_DISABLE", 0, "Precision Off", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_ensure(
      keyconf, "Object Orbit Around Target Modal Map", modal_items);
  WM_modalkeymap_assign(keymap, "OBJECT_OT_orbit_around_target");
}

static void object_orbit_around_target_cancel(bContext *C, wmOperator *op)
{
  ObjectOrbitAroundTargetData *ooatd = static_cast<ObjectOrbitAroundTargetData *>(op->customdata);

  /* Restore original positions/rotations. */
  for (const ObjectOrbitAroundTargetData::ObjectData &light : ooatd->objects) {
    copy_v3_v3(light.ob->loc, light.orig_loc);

    /* Restore rotation data in the original format to avoid lossy conversions. */
    if (light.ob->rotmode == ROT_MODE_QUAT) {
      copy_v4_v4(light.ob->quat, light.orig_quat);
    }
    else if (light.ob->rotmode == ROT_MODE_AXISANGLE) {
      copy_v3_v3(light.ob->rotAxis, light.orig_rot_axis);
      light.ob->rotAngle = light.orig_rot_angle;
    }
    else {
      copy_v3_v3(light.ob->rot, light.orig_rot);
    }

    DEG_id_tag_update(&light.ob->id, ID_RECALC_TRANSFORM);
    WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, light.ob);
  }

  /* Restore default cursor. */
  WM_cursor_set(CTX_wm_window(C), WM_CURSOR_DEFAULT);

  ED_region_tag_redraw(ooatd->vc.region);
  ED_workspace_status_text(C, nullptr);

  /* Free navigation data */
  if (ooatd->vod) {
    ED_view3d_navigation_free(C, ooatd->vod);
  }

  MEM_delete(ooatd);
}

static void object_set_rotation_from_matrix(Object *ob,
                                            const float mat[3][3],
                                            const float orig_rot[3])
{
  if (ob->rotmode == ROT_MODE_QUAT) {
    mat3_to_quat(ob->quat, mat);
  }
  else if (ob->rotmode == ROT_MODE_AXISANGLE) {
    mat3_to_axis_angle(ob->rotAxis, &ob->rotAngle, mat);
  }
  else {
    float3 euler;
    mat3_to_compatible_eulO(euler, orig_rot, ob->rotmode, mat);
    copy_v3_v3(ob->rot, euler);
  }
}

static void light_orbit_direction(float result[3], float azimuth, float elevation)
{
  result[0] = cosf(elevation) * sinf(azimuth);
  result[1] = cosf(elevation) * cosf(azimuth);
  result[2] = sinf(elevation);
}

static void light_orbit_update_position(
    Object *light_ob, const float center[3], float azimuth, float elevation, float distance)
{
  float3 new_dir;
  light_orbit_direction(new_dir, azimuth, elevation);

  /* Handle negative distances with automatic direction inversion. */
  float actual_distance = distance;
  bool should_invert = (actual_distance < 0.0f);
  if (should_invert) {
    actual_distance = -actual_distance; /* Use positive distance for positioning. */
    negate_v3(new_dir);                 /* Invert direction vector. */
  }

  /* Set new position using current distance and direction. */
  madd_v3_v3v3fl(light_ob->loc, center, new_dir, actual_distance);
}

static void light_orbit_update_rotation(Object *light_ob,
                                        const float center[3],
                                        const float orig_rot[3])
{
  /* Update rotation to point at center. */
  float3 target_dir;
  sub_v3_v3v3(target_dir, center, light_ob->loc);
  normalize_v3(target_dir);

  /* Convert direction vector to euler angles for light pointing. */
  /* In Blender, lights point in negative Z direction by default. */
  /* So we need to orient the light so its -Z axis points toward target. */
  float3 up = {0.0f, 0.0f, 1.0f};
  float3 right;
  cross_v3_v3v3(right, target_dir, up);
  normalize_v3(right);
  cross_v3_v3v3(up, right, target_dir);

  /* Create rotation matrix and convert to rotation mode. */
  float rot_mat[3][3];
  copy_v3_v3(rot_mat[0], right);
  copy_v3_v3(rot_mat[1], up);
  negate_v3_v3(rot_mat[2], target_dir); /* -Z points toward target. */

  object_set_rotation_from_matrix(light_ob, rot_mat, orig_rot);
}

static wmOperatorStatus object_orbit_around_target_invoke(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent *event)
{
  object_orbit_around_target_init_data(C, op, event);
  ObjectOrbitAroundTargetData *ooatd = static_cast<ObjectOrbitAroundTargetData *>(op->customdata);

  if (ooatd->objects.is_empty()) {
    MEM_delete(ooatd);
    op->customdata = nullptr;
    ED_workspace_status_text(C, nullptr);
    return OPERATOR_CANCELLED;
  }

  /* Initialize viewport navigation */
  ooatd->vod = ED_view3d_navigation_init(C, nullptr);

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static void object_orbit_around_target_confirm(bContext *C, wmOperator *op)
{
  ObjectOrbitAroundTargetData *ooatd = static_cast<ObjectOrbitAroundTargetData *>(op->customdata);

  Scene *scene = CTX_data_scene(C);
  for (const ObjectOrbitAroundTargetData::ObjectData &light : ooatd->objects) {
    PointerRNA ptr = RNA_pointer_create_discrete(&light.ob->id, RNA_Object, &light.ob->id);

    PropertyRNA *prop_loc = RNA_struct_find_property(&ptr, "location");
    animrig::autokeyframe_property(C, scene, &ptr, prop_loc, -1, scene->r.cfra, true);

    const char *rotation_property = "rotation_euler";
    if (light.ob->rotmode == ROT_MODE_QUAT) {
      rotation_property = "rotation_quaternion";
    }
    else if (light.ob->rotmode == ROT_MODE_AXISANGLE) {
      rotation_property = "rotation_axis_angle";
    }
    PropertyRNA *prop_rot = RNA_struct_find_property(&ptr, rotation_property);
    animrig::autokeyframe_property(C, scene, &ptr, prop_rot, -1, scene->r.cfra, true);
  }

  WM_cursor_set(CTX_wm_window(C), WM_CURSOR_DEFAULT);
  ED_workspace_status_text(C, nullptr);

  if (ooatd->vod) {
    ED_view3d_navigation_free(C, ooatd->vod);
  }
  MEM_delete(ooatd);
  op->customdata = nullptr;
}

static wmOperatorStatus object_orbit_around_target_modal(bContext *C,
                                                         wmOperator *op,
                                                         const wmEvent *event)
{
  ObjectOrbitAroundTargetData *ooatd = static_cast<ObjectOrbitAroundTargetData *>(op->customdata);

  /* Handle navigation events */
  if (ooatd->vod && ED_view3d_navigation_do(C, ooatd->vod, event, nullptr)) {
    ooatd->run_navigation = true;
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == MOUSEMOVE) {
    if (ooatd->has_center) {
      float delta_x = float(event->mval[0]) - ooatd->current_mval[0];
      float delta_y = float(event->mval[1]) - ooatd->current_mval[1];

      /* Convert mouse movement to azimuth and elevation. */
      float sensitivity = 0.01f;
      if (ooatd->precision_mode) {
        sensitivity *= ooatd->precision_factor;
      }
      float azimuth_delta = delta_x * sensitivity;
      float elevation_delta = delta_y * sensitivity;

      /* Apply axis locking. */
      if (ooatd->axis_lock == ORB_AXIS_LOCK_AZIMUTH) {
        elevation_delta = 0.0f; /* Lock elevation, only allow azimuth. */
      }
      else if (ooatd->axis_lock == ORB_AXIS_LOCK_ELEVATION) {
        azimuth_delta = 0.0f; /* Lock azimuth, only allow elevation. */
      }
      else if (ooatd->axis_lock == ORB_AXIS_LOCK_DISTANCE) {
        azimuth_delta = 0.0f;   /* Lock azimuth, only allow distance. */
        elevation_delta = 0.0f; /* Lock elevation, only allow distance. */
      }

      for (ObjectOrbitAroundTargetData::ObjectData &light : ooatd->objects) {
        if (ooatd->axis_lock == ORB_AXIS_LOCK_DISTANCE) {
          /* Distance mode: only change distance, keep direction. */
          float sensitivity = 0.1f;
          if (ooatd->precision_mode) {
            sensitivity *= ooatd->precision_factor;
          }
          float distance_delta = delta_y *
                                 sensitivity; /* Vertical mouse movement controls distance. */
          light.current_distance += distance_delta;
          /* Allow negative distances for traversing intersection point. */
        }
        else {
          /* Normal mode: update azimuth and elevation. */
          light.current_azimuth += azimuth_delta;
          light.current_elevation += elevation_delta;
          light.current_elevation = clamp_f(
              light.current_elevation, -M_PI_2 + 0.001f, M_PI_2 - 0.001f);
        }

        /* Update position and rotation. */
        light_orbit_update_position(light.ob,
                                    ooatd->center,
                                    light.current_azimuth,
                                    light.current_elevation,
                                    light.current_distance);
        light_orbit_update_rotation(light.ob, ooatd->center, light.orig_rot);

        DEG_id_tag_update(&light.ob->id, ID_RECALC_TRANSFORM);
        WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, light.ob);
      }

      /* Update current mouse position as the new reference point. */
      ooatd->current_mval[0] = float(event->mval[0]);
      ooatd->current_mval[1] = float(event->mval[1]);

      ED_region_tag_redraw(ooatd->vc.region);
    }
  }

  /* Handle modal keymap events. */
  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case ORB_MODAL_CONFIRM:
        object_orbit_around_target_confirm(C, op);
        return OPERATOR_FINISHED;

      case ORB_MODAL_CANCEL:
        object_orbit_around_target_cancel(C, op);
        return OPERATOR_CANCELLED;

      case ORB_MODAL_SWITCH_TO_TARGET:
        object_orbit_around_target_confirm(C, op);
        WM_operator_name_call(C,
                              "OBJECT_OT_transform_axis_target",
                              wm::OpCallContext::InvokeDefault,
                              nullptr,
                              nullptr);
        return OPERATOR_FINISHED;

      case ORB_MODAL_AZIMUTH_LOCK: {
        /* Toggle azimuth lock (horizontal movement only). */
        if (ooatd->axis_lock == ORB_AXIS_LOCK_AZIMUTH) {
          ooatd->axis_lock = ORB_AXIS_LOCK_NONE; /* Turn off lock. */
        }
        else {
          ooatd->axis_lock = ORB_AXIS_LOCK_AZIMUTH; /* Lock to azimuth only. */
        }
        /* Update reference point to current mouse position. */
        ooatd->current_mval[0] = float(event->mval[0]);
        ooatd->current_mval[1] = float(event->mval[1]);
        /* Set appropriate cursor. */
        object_orbit_around_target_set_cursor(C, ooatd);
        /* Update status text. */
        object_orbit_around_target_update_status(C, op, ooatd);
        ED_region_tag_redraw(ooatd->vc.region);
        return OPERATOR_RUNNING_MODAL;
      }

      case ORB_MODAL_ELEVATION_LOCK: {
        /* Toggle elevation lock (vertical movement only). */
        if (ooatd->axis_lock == ORB_AXIS_LOCK_ELEVATION) {
          ooatd->axis_lock = ORB_AXIS_LOCK_NONE; /* Turn off lock. */
        }
        else {
          ooatd->axis_lock = ORB_AXIS_LOCK_ELEVATION; /* Lock to elevation only. */
        }
        /* Update reference point to current mouse position. */
        ooatd->current_mval[0] = float(event->mval[0]);
        ooatd->current_mval[1] = float(event->mval[1]);
        /* Set appropriate cursor. */
        object_orbit_around_target_set_cursor(C, ooatd);
        /* Update status text. */
        object_orbit_around_target_update_status(C, op, ooatd);
        ED_region_tag_redraw(ooatd->vc.region);
        return OPERATOR_RUNNING_MODAL;
      }

      case ORB_MODAL_DISTANCE_LOCK: {
        /* Toggle distance lock (distance movement only). */
        if (ooatd->axis_lock == ORB_AXIS_LOCK_DISTANCE) {
          ooatd->axis_lock = ORB_AXIS_LOCK_NONE; /* Turn off lock. */
        }
        else {
          ooatd->axis_lock = ORB_AXIS_LOCK_DISTANCE; /* Lock to distance only. */
        }
        /* Update reference point to current mouse position. */
        ooatd->current_mval[0] = float(event->mval[0]);
        ooatd->current_mval[1] = float(event->mval[1]);
        /* Set appropriate cursor. */
        object_orbit_around_target_set_cursor(C, ooatd);
        /* Update status text. */
        object_orbit_around_target_update_status(C, op, ooatd);
        ED_region_tag_redraw(ooatd->vc.region);
        return OPERATOR_RUNNING_MODAL;
      }

      case ORB_MODAL_INVERT: {
        /* Invert light direction by rotating 180° around local Y axis. */
        for (ObjectOrbitAroundTargetData::ObjectData &light : ooatd->objects) {
          /* Get current rotation matrix. */
          float current_rot_mat[3][3];
          if (light.ob->rotmode == ROT_MODE_QUAT) {
            quat_to_mat3(current_rot_mat, light.ob->quat);
          }
          else if (light.ob->rotmode == ROT_MODE_AXISANGLE) {
            axis_angle_to_mat3(current_rot_mat, light.ob->rotAxis, light.ob->rotAngle);
          }
          else {
            eulO_to_mat3(current_rot_mat, light.ob->rot, light.ob->rotmode);
          }

          /* Create 180° rotation around Y axis. */
          float y_rot_180[3][3];
          unit_m3(y_rot_180);
          y_rot_180[0][0] = -1.0f; /* Flip X. */
          y_rot_180[2][2] = -1.0f; /* Flip Z. */
          /* Y stays the same. */

          /* Apply the rotation. */
          float new_rot_mat[3][3];
          mul_m3_m3m3(new_rot_mat, current_rot_mat, y_rot_180);

          /* Convert back to light's rotation mode. */
          object_set_rotation_from_matrix(light.ob, new_rot_mat, light.orig_rot);

          DEG_id_tag_update(&light.ob->id, ID_RECALC_TRANSFORM);
          WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, light.ob);
        }

        ED_region_tag_redraw(ooatd->vc.region);
        return OPERATOR_RUNNING_MODAL;
      }

      case ORB_MODAL_SYMMETRY: {
        /* Symmetry around intersection point. */
        for (ObjectOrbitAroundTargetData::ObjectData &light : ooatd->objects) {
          light.direction_inverted = !light.direction_inverted;

          /* Apply symmetry by flipping the spherical coordinates. */
          light.current_azimuth += M_PI;                      /* Rotate 180 degrees in azimuth. */
          light.current_elevation = -light.current_elevation; /* Flip elevation. */

          /* Normalize azimuth to [-PI, PI] range. */
          if (light.current_azimuth > M_PI) {
            light.current_azimuth -= 2.0f * M_PI;
          }
          else if (light.current_azimuth < -M_PI) {
            light.current_azimuth += 2.0f * M_PI;
          }

          /* Recalculate position and orientation. */
          float3 new_dir;
          new_dir[0] = cosf(light.current_elevation) * sinf(light.current_azimuth);
          new_dir[1] = cosf(light.current_elevation) * cosf(light.current_azimuth);
          new_dir[2] = sinf(light.current_elevation);

          /* Set new position using current distance. */
          float actual_distance = fabsf(light.current_distance);
          madd_v3_v3v3fl(light.ob->loc, ooatd->center, new_dir, actual_distance);

          /* Update light orientation to point toward center. */
          float3 target_dir;
          negate_v3_v3(target_dir, new_dir);

          float3 up = {0.0f, 0.0f, 1.0f};
          float3 right;
          cross_v3_v3v3(right, target_dir, up);
          normalize_v3(right);
          cross_v3_v3v3(up, right, target_dir);

          float rot_mat[3][3];
          copy_v3_v3(rot_mat[0], right);
          copy_v3_v3(rot_mat[1], up);
          negate_v3_v3(rot_mat[2], target_dir);

          /* Convert back to light's rotation mode. */
          object_set_rotation_from_matrix(light.ob, rot_mat, light.orig_rot);

          DEG_id_tag_update(&light.ob->id, ID_RECALC_TRANSFORM);
          WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, light.ob);
        }

        /* Update status text. */
        object_orbit_around_target_update_status(C, op, ooatd);
        ED_region_tag_redraw(ooatd->vc.region);
        return OPERATOR_RUNNING_MODAL;
      }

      case ORB_MODAL_PRECISION_ENABLE: {
        /* Enable precision mode. */
        if (!ooatd->precision_mode) {
          ooatd->precision_mode = true;
          ooatd->precision_toggle_mval[0] = float(event->mval[0]);
          ooatd->precision_toggle_mval[1] = float(event->mval[1]);
          /* Update status text. */
          object_orbit_around_target_update_status(C, op, ooatd);
          ED_region_tag_redraw(ooatd->vc.region);
        }
        return OPERATOR_RUNNING_MODAL;
      }

      case ORB_MODAL_PRECISION_DISABLE: {
        /* Disable precision mode. */
        if (ooatd->precision_mode) {
          ooatd->precision_mode = false;
          /* Update reference point to current mouse position for smooth transition. */
          ooatd->current_mval[0] = float(event->mval[0]);
          ooatd->current_mval[1] = float(event->mval[1]);
          /* Update status text. */
          object_orbit_around_target_update_status(C, op, ooatd);
          ED_region_tag_redraw(ooatd->vc.region);
        }
        return OPERATOR_RUNNING_MODAL;
      }
    }
  }

  return OPERATOR_RUNNING_MODAL;
}

static bool object_orbit_around_target_poll(bContext *C)
{
  if (!ED_operator_region_view3d_active(C)) {
    return false;
  }

  /* Should not be used in edit mode. */
  if (CTX_data_edit_object(C)) {
    return false;
  }

  /* Check if active object is a light or camera. */
  Object *active_ob = CTX_data_active_object(C);
  if (active_ob && ELEM(active_ob->type, OB_LAMP, OB_CAMERA)) {
    return true;
  }

  return false;
}

void OBJECT_OT_orbit_around_target(wmOperatorType *ot)
{
  /* identifiers. */
  ot->name = "Object Orbit Around Target";
  ot->description = "Interactively orbit object around the point where it targets geometry";
  ot->idname = "OBJECT_OT_orbit_around_target";

  /* API callbacks. */
  ot->invoke = object_orbit_around_target_invoke;
  ot->cancel = object_orbit_around_target_cancel;
  ot->modal = object_orbit_around_target_modal;
  ot->poll = object_orbit_around_target_poll;

  /* flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/** \}. */

}  // namespace blender::ed::object
