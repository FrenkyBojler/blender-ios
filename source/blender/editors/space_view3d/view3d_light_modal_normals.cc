/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 *
 * Modal operator for interactive light positioning using raycast.
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_utildefines.h"

#include "BKE_context.hh"
#include "BKE_light.h"
#include "BKE_object.hh"
#include "BKE_scene.hh"

#include "DEG_depsgraph.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"
#include "ED_transform_snap_object_context.hh"
#include "ED_view3d.hh"

#include "UI_resources.hh"

#include "view3d_intern.hh"

/* -------------------------------------------------------------------- */
/** \name Light Normal Positioning Modal Operator
 * \{ */

/* Forward declaration. */
static void light_modal_cleanup(bContext *C, wmOperator *op);

struct LightModalData {
  /** Light object being positioned. */
  Object *light_ob;
  /** Mouse position. */
  int mval[2];
  /** Snap context for raycasting. */
  blender::ed::transform::SnapObjectContext *snap_context;
  /** Dependency graph. */
  Depsgraph *depsgraph;
  /** View3D context. */
  View3D *v3d;
  /** 3D region. */
  ARegion *region;
  /** Region view 3D. */
  RegionView3D *rv3d;
  /** Timer for updates. */
  wmTimer *timer;
  /** Current offset distance from surface. */
  float offset_distance;
  /** Whether Z-axis adjustment mode is active. */
  bool z_adjust_mode;
  /** Fixed hit position during Z adjustment. */
  float fixed_hit_location[3];
  /** Initial mouse X position when Z adjustment started. */
  int z_adjust_initial_mval_x;
  /** Light position when Z adjustment started. */
  float z_adjust_initial_light_pos[3];
  /** Light local Z direction when Z adjustment started. */
  float light_local_z[3];
};

static void light_modal_update_position(bContext *C, LightModalData *lmd)
{
  if (!lmd->light_ob || lmd->light_ob->type != OB_LAMP) {
    return;
  }

  if (lmd->z_adjust_mode) {
    /* Z-axis adjustment mode: move light along its local Z-axis using left/right mouse movement */
    float mouse_delta_x = float(lmd->mval[0] - lmd->z_adjust_initial_mval_x);
    float z_offset = mouse_delta_x * 0.05f; /* Scale factor for sensitivity */
    
    /* Position light at initial position + Z offset along stored local Z direction */
    float final_location[3];
    madd_v3_v3v3fl(final_location, lmd->z_adjust_initial_light_pos, lmd->light_local_z, z_offset);
    copy_v3_v3(lmd->light_ob->loc, final_location);
  }
  else {
    /* Normal positioning mode */
    float ray_start[3], ray_normal[3];
    float mval_fl[2] = {float(lmd->mval[0]), float(lmd->mval[1])};
    
    ED_view3d_win_to_ray(lmd->region, mval_fl, ray_start, ray_normal);

    /* Perform raycast using snap system */
    blender::ed::transform::SnapObjectParams snap_params = {};
    snap_params.snap_target_select = SCE_SNAP_TARGET_ALL;

    float hit_location[3], hit_normal[3];
    bool hit = blender::ed::transform::snap_object_project_ray(lmd->snap_context,
                                                              lmd->depsgraph,
                                                              lmd->v3d,
                                                              &snap_params,
                                                              ray_start,
                                                              ray_normal,
                                                              nullptr,
                                                              hit_location,
                                                              hit_normal);

    if (hit) {
      /* Normal mode: position with offset along surface normal */
      float final_location[3];
      madd_v3_v3v3fl(final_location, hit_location, hit_normal, lmd->offset_distance);
      copy_v3_v3(lmd->light_ob->loc, final_location);

      /* Orient light toward hit location */
      float direction_to_hit[3];
      sub_v3_v3v3(direction_to_hit, hit_location, final_location);
      normalize_v3(direction_to_hit);
      
      float z_axis[3] = {0.0f, 0.0f, -1.0f}; /* Light forward direction */
      float quat[4];
      rotation_between_vecs_to_quat(quat, z_axis, direction_to_hit);
      quat_to_eul(lmd->light_ob->rot, quat);
    }
  }

  /* Tag for update */
  DEG_id_tag_update(&lmd->light_ob->id, ID_RECALC_TRANSFORM);
  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, lmd->light_ob);
}

static wmOperatorStatus light_normal_positioning_invoke(bContext *C,
                                                       wmOperator *op,
                                                       const wmEvent *event)
{
  /* Check context. */
  if (CTX_wm_view3d(C) == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Must be in 3D View");
    return OPERATOR_CANCELLED;
  }

  Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_LAMP) {
    BKE_report(op->reports, RPT_ERROR, "No light object selected");
    return OPERATOR_CANCELLED;
  }

  /* Allocate modal data. */
  LightModalData *lmd = static_cast<LightModalData *>(MEM_callocN(sizeof(LightModalData), __func__));
  
  /* Initialize modal data. */
  lmd->light_ob = ob;
  lmd->mval[0] = event->mval[0];
  lmd->mval[1] = event->mval[1];
  lmd->region = CTX_wm_region(C);
  lmd->rv3d = CTX_wm_region_view3d(C);
  lmd->v3d = CTX_wm_view3d(C);
  lmd->depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  lmd->z_adjust_mode = false;
  
  /* Create snap context. */
  lmd->snap_context = blender::ed::transform::snap_object_context_create(CTX_data_scene(C), 0);
  
  /* Calculate initial offset distance from current light position */
  float ray_start[3], ray_normal[3];
  float mval_fl[2] = {float(lmd->mval[0]), float(lmd->mval[1])};
  ED_view3d_win_to_ray(lmd->region, mval_fl, ray_start, ray_normal);
  
  blender::ed::transform::SnapObjectParams snap_params = {};
  snap_params.snap_target_select = SCE_SNAP_TARGET_ALL;
  
  float hit_location[3], hit_normal[3];
  bool hit = blender::ed::transform::snap_object_project_ray(lmd->snap_context,
                                                            lmd->depsgraph,
                                                            lmd->v3d,
                                                            &snap_params,
                                                            ray_start,
                                                            ray_normal,
                                                            nullptr,
                                                            hit_location,
                                                            hit_normal);
  
  if (hit) {
    /* Use current distance between light and hit as initial offset */
    float distance_vec[3];
    sub_v3_v3v3(distance_vec, lmd->light_ob->loc, hit_location);
    lmd->offset_distance = len_v3(distance_vec);
  } else {
    /* Fallback to default distance */
    lmd->offset_distance = 4.0f;
  }

  /* Add timer. */
  wmWindowManager *wm = CTX_wm_manager(C);
  lmd->timer = WM_event_timer_add(wm, CTX_wm_window(C), TIMER, 0.02f);

  /* Set custom data. */
  op->customdata = lmd;

  /* Change cursor. */
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);

  /* Add modal handler. */
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus light_normal_positioning_modal(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event)
{
  LightModalData *lmd = static_cast<LightModalData *>(op->customdata);

  switch (event->type) {
    case MOUSEMOVE:
      /* Update mouse position and light location. */
      lmd->mval[0] = event->mval[0];
      lmd->mval[1] = event->mval[1];
      light_modal_update_position(C, lmd);
      break;

    case LEFTMOUSE:
      if (event->val == KM_PRESS) {
        /* Confirm placement. */
        light_modal_cleanup(C, op);
        return OPERATOR_FINISHED;
      }
      break;

    case EVT_ZKEY:
      if (event->val == KM_PRESS) {
        /* Start Z-axis adjustment mode */
        lmd->z_adjust_mode = true;
        lmd->z_adjust_initial_mval_x = lmd->mval[0];
        copy_v3_v3(lmd->z_adjust_initial_light_pos, lmd->light_ob->loc);
        
        /* Calculate light's local Z axis from its current rotation */
        float light_matrix[4][4];
        loc_eul_size_to_mat4(light_matrix, 
                             lmd->light_ob->loc, 
                             lmd->light_ob->rot, 
                             lmd->light_ob->scale);
        
        /* Extract local Z axis (negative Z for light forward direction) */
        lmd->light_local_z[0] = -light_matrix[2][0];
        lmd->light_local_z[1] = -light_matrix[2][1];
        lmd->light_local_z[2] = -light_matrix[2][2];
      }
      else if (event->val == KM_RELEASE) {
        /* End Z-axis adjustment mode and update offset distance */
        lmd->z_adjust_mode = false;
        
        /* Calculate new offset distance from current position to last hit */
        float ray_start[3], ray_normal[3];
        float mval_fl[2] = {float(lmd->mval[0]), float(lmd->mval[1])};
        ED_view3d_win_to_ray(lmd->region, mval_fl, ray_start, ray_normal);
        
        blender::ed::transform::SnapObjectParams snap_params = {};
        snap_params.snap_target_select = SCE_SNAP_TARGET_ALL;
        
        float hit_location[3], hit_normal[3];
        bool hit = blender::ed::transform::snap_object_project_ray(lmd->snap_context,
                                                                  lmd->depsgraph,
                                                                  lmd->v3d,
                                                                  &snap_params,
                                                                  ray_start,
                                                                  ray_normal,
                                                                  nullptr,
                                                                  hit_location,
                                                                  hit_normal);
        
        if (hit) {
          float distance_vec[3];
          sub_v3_v3v3(distance_vec, lmd->light_ob->loc, hit_location);
          lmd->offset_distance = len_v3(distance_vec);
        }
        
        /* Resume normal positioning */
        light_modal_update_position(C, lmd);
      }
      break;

    case EVT_ESCKEY:
    case RIGHTMOUSE:
      if (event->val == KM_PRESS) {
        /* Cancel operation. */
        light_modal_cleanup(C, op);
        return OPERATOR_CANCELLED;
      }
      break;

    case MIDDLEMOUSE:
    case WHEELUPMOUSE:
    case WHEELDOWNMOUSE:
      /* Pass through for navigation. */
      return OPERATOR_PASS_THROUGH;

    case TIMER:
      /* Periodic update. */
      light_modal_update_position(C, lmd);
      break;

    default:
      /* Ignore other events. */
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static void light_modal_cleanup(bContext *C, wmOperator *op)
{
  LightModalData *lmd = static_cast<LightModalData *>(op->customdata);
  
  if (lmd) {
    /* Clean up resources. */
    if (lmd->snap_context) {
      blender::ed::transform::snap_object_context_destroy(lmd->snap_context);
    }
    if (lmd->timer) {
      wmWindowManager *wm = CTX_wm_manager(C);
      WM_event_timer_remove(wm, CTX_wm_window(C), lmd->timer);
    }
    
    MEM_freeN(lmd);
    op->customdata = nullptr;
  }

  /* Restore cursor. */
  WM_cursor_modal_restore(CTX_wm_window(C));
}

static void light_normal_positioning_cancel(bContext *C, wmOperator *op)
{
  light_modal_cleanup(C, op);
}

static wmOperatorStatus light_normal_positioning_exec(bContext * /*C*/, wmOperator * /*op*/)
{
  /* Not used - this is a modal operator. */
  return OPERATOR_CANCELLED;
}

void VIEW3D_OT_light_normal_positioning(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Raycast light from normals";
  ot->description = "Interactively position a light based on surface normals using mouse raycast";
  ot->idname = "VIEW3D_OT_light_normal_positioning";

  /* API callbacks. */
  ot->invoke = light_normal_positioning_invoke;
  ot->modal = light_normal_positioning_modal;
  ot->cancel = light_normal_positioning_cancel;
  ot->exec = light_normal_positioning_exec;
  ot->poll = ED_operator_region_view3d_active;

  /* Flags. */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/** \} */