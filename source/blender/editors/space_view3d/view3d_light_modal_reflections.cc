/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"

#include "BKE_context.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph_query.hh"

#include "ED_view3d.hh"
#include "ED_transform_snap_object_context.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "view3d_intern.hh"

/** \name Light Reflection Positioning Modal Operator
 * \{ */

/* Forward declaration. */
static void light_reflection_cleanup(bContext *C, wmOperator *op);

struct LightReflectionData {
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

static void light_reflection_update_position(bContext *C, LightReflectionData *lrd)
{
  if (!lrd->light_ob || lrd->light_ob->type != OB_LAMP) {
    return;
  }

  if (lrd->z_adjust_mode) {
    /* Z-axis adjustment mode: move light along its local Z-axis using left/right mouse movement */
    float mouse_delta_x = float(lrd->mval[0] - lrd->z_adjust_initial_mval_x);
    float z_offset = mouse_delta_x * 0.05f; /* Scale factor for sensitivity */
    
    /* Position light at initial position + Z offset along stored local Z direction */
    float final_location[3];
    madd_v3_v3v3fl(final_location, lrd->z_adjust_initial_light_pos, lrd->light_local_z, z_offset);
    copy_v3_v3(lrd->light_ob->loc, final_location);
  }
  else {
    /* Normal reflection positioning mode */
    float ray_start[3], ray_normal[3];
    float mval_fl[2] = {float(lrd->mval[0]), float(lrd->mval[1])};
    
    ED_view3d_win_to_ray(lrd->region, mval_fl, ray_start, ray_normal);

    /* Perform raycast using snap system */
    blender::ed::transform::SnapObjectParams snap_params = {};
    snap_params.snap_target_select = SCE_SNAP_TARGET_ALL;

    float hit_location[3], hit_normal[3];
    bool hit = blender::ed::transform::snap_object_project_ray(lrd->snap_context,
                                                              lrd->depsgraph,
                                                              lrd->v3d,
                                                              &snap_params,
                                                              ray_start,
                                                              ray_normal,
                                                              nullptr,
                                                              hit_location,
                                                              hit_normal);

    if (hit) {
      /* Calculate incident vector (from ray origin to hit location) */
      float incident_vec[3];
      sub_v3_v3v3(incident_vec, ray_start, hit_location);
      normalize_v3(incident_vec);
      
      /* Calculate reflection vector */
      float reflection_vec[3];
      reflect_v3_v3v3(reflection_vec, incident_vec, hit_normal);
      
      /* Position light opposite to reflection direction with dynamic offset */
      float final_location[3];
      mul_v3_v3fl(final_location, reflection_vec, -lrd->offset_distance);
      add_v3_v3(final_location, hit_location);
      copy_v3_v3(lrd->light_ob->loc, final_location);
      
      /* Orient light to point towards hit location */
      float direction_to_hit[3];
      sub_v3_v3v3(direction_to_hit, hit_location, final_location);
      normalize_v3(direction_to_hit);
      
      float z_axis[3] = {0.0f, 0.0f, -1.0f}; /* Light forward direction */
      float quat[4];
      rotation_between_vecs_to_quat(quat, z_axis, direction_to_hit);
      quat_to_eul(lrd->light_ob->rot, quat);
    }
  }

  /* Tag for update */
  DEG_id_tag_update(&lrd->light_ob->id, ID_RECALC_TRANSFORM);
  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, lrd->light_ob);
}

static wmOperatorStatus light_reflection_positioning_invoke(bContext *C,
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
  LightReflectionData *lrd = static_cast<LightReflectionData *>(MEM_callocN(sizeof(LightReflectionData), __func__));
  
  /* Initialize modal data. */
  lrd->light_ob = ob;
  lrd->mval[0] = event->mval[0];
  lrd->mval[1] = event->mval[1];
  lrd->region = CTX_wm_region(C);
  lrd->rv3d = CTX_wm_region_view3d(C);
  lrd->v3d = CTX_wm_view3d(C);
  lrd->depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  lrd->z_adjust_mode = false;

  /* Create snap context. */
  lrd->snap_context = blender::ed::transform::snap_object_context_create(CTX_data_scene(C), 0);
  
  /* Calculate initial offset distance from current light position */
  float ray_start[3], ray_normal[3];
  float mval_fl[2] = {float(lrd->mval[0]), float(lrd->mval[1])};
  ED_view3d_win_to_ray(lrd->region, mval_fl, ray_start, ray_normal);
  
  blender::ed::transform::SnapObjectParams snap_params = {};
  snap_params.snap_target_select = SCE_SNAP_TARGET_ALL;
  
  float hit_location[3], hit_normal[3];
  bool hit = blender::ed::transform::snap_object_project_ray(lrd->snap_context,
                                                            lrd->depsgraph,
                                                            lrd->v3d,
                                                            &snap_params,
                                                            ray_start,
                                                            ray_normal,
                                                            nullptr,
                                                            hit_location,
                                                            hit_normal);
  
  if (hit) {
    /* Use current distance between light and hit as initial offset */
    float distance_vec[3];
    sub_v3_v3v3(distance_vec, lrd->light_ob->loc, hit_location);
    lrd->offset_distance = len_v3(distance_vec);
  } else {
    /* Fallback to default distance */
    lrd->offset_distance = 4.0f;
  }

  /* Add timer. */
  wmWindowManager *wm = CTX_wm_manager(C);
  lrd->timer = WM_event_timer_add(wm, CTX_wm_window(C), TIMER, 0.02f);

  /* Set custom data. */
  op->customdata = lrd;

  /* Set modal cursor. */
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);

  /* Add modal handler. */
  WM_event_add_modal_handler(C, op);

  /* Initial position update. */
  light_reflection_update_position(C, lrd);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus light_reflection_positioning_modal(bContext *C,
                                                          wmOperator *op,
                                                          const wmEvent *event)
{
  LightReflectionData *lrd = static_cast<LightReflectionData *>(op->customdata);

  switch (event->type) {
    case MOUSEMOVE:
      lrd->mval[0] = event->mval[0];
      lrd->mval[1] = event->mval[1];
      light_reflection_update_position(C, lrd);
      break;
    
    case LEFTMOUSE:
      if (event->val == KM_PRESS) {
        light_reflection_cleanup(C, op);
        return OPERATOR_FINISHED;
      }
      break;
    
    case EVT_ZKEY:
      if (event->val == KM_PRESS) {
        /* Start Z-axis adjustment mode */
        lrd->z_adjust_mode = true;
        lrd->z_adjust_initial_mval_x = lrd->mval[0];
        copy_v3_v3(lrd->z_adjust_initial_light_pos, lrd->light_ob->loc);
        
        /* Calculate light's local Z axis from its current rotation */
        float light_matrix[4][4];
        loc_eul_size_to_mat4(light_matrix, 
                             lrd->light_ob->loc, 
                             lrd->light_ob->rot, 
                             lrd->light_ob->scale);
        
        /* Extract local Z axis (negative Z for light forward direction) */
        lrd->light_local_z[0] = -light_matrix[2][0];
        lrd->light_local_z[1] = -light_matrix[2][1];
        lrd->light_local_z[2] = -light_matrix[2][2];
      }
      else if (event->val == KM_RELEASE) {
        /* End Z-axis adjustment mode and update offset distance */
        lrd->z_adjust_mode = false;
        
        /* Calculate new offset distance from current position to last hit */
        float ray_start[3], ray_normal[3];
        float mval_fl[2] = {float(lrd->mval[0]), float(lrd->mval[1])};
        ED_view3d_win_to_ray(lrd->region, mval_fl, ray_start, ray_normal);
        
        blender::ed::transform::SnapObjectParams snap_params = {};
        snap_params.snap_target_select = SCE_SNAP_TARGET_ALL;
        
        float hit_location[3], hit_normal[3];
        bool hit = blender::ed::transform::snap_object_project_ray(lrd->snap_context,
                                                                  lrd->depsgraph,
                                                                  lrd->v3d,
                                                                  &snap_params,
                                                                  ray_start,
                                                                  ray_normal,
                                                                  nullptr,
                                                                  hit_location,
                                                                  hit_normal);
        
        if (hit) {
          float distance_vec[3];
          sub_v3_v3v3(distance_vec, lrd->light_ob->loc, hit_location);
          lrd->offset_distance = len_v3(distance_vec);
        }
        
        /* Resume normal positioning */
        light_reflection_update_position(C, lrd);
      }
      break;

    case EVT_ESCKEY:
      light_reflection_cleanup(C, op);
      return OPERATOR_CANCELLED;
      
    case MIDDLEMOUSE:
    case WHEELUPMOUSE:
    case WHEELDOWNMOUSE:
      return OPERATOR_PASS_THROUGH;

    case TIMER:
      /* Update position periodically. */
      light_reflection_update_position(C, lrd);
      break;

    default:
      /* Ignore other events. */
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static void light_reflection_cleanup(bContext *C, wmOperator *op)
{
  LightReflectionData *lrd = static_cast<LightReflectionData *>(op->customdata);
  
  if (lrd) {
    /* Clean up resources. */
    if (lrd->snap_context) {
      blender::ed::transform::snap_object_context_destroy(lrd->snap_context);
    }
    if (lrd->timer) {
      wmWindowManager *wm = CTX_wm_manager(C);
      WM_event_timer_remove(wm, CTX_wm_window(C), lrd->timer);
    }
    
    MEM_freeN(lrd);
    op->customdata = nullptr;
  }

  /* Restore cursor. */
  WM_cursor_modal_restore(CTX_wm_window(C));
}

static void light_reflection_positioning_cancel(bContext *C, wmOperator *op)
{
  light_reflection_cleanup(C, op);
}

void VIEW3D_OT_light_reflection_positioning(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Light Reflection Positioning";
  ot->description = "Interactively position light based on specular reflection";
  ot->idname = "VIEW3D_OT_light_reflection_positioning";

  /* api callbacks */
  ot->invoke = light_reflection_positioning_invoke;
  ot->modal = light_reflection_positioning_modal;
  ot->cancel = light_reflection_positioning_cancel;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/** \} */