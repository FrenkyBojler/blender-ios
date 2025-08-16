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
#include "ED_screen.hh"
#include "ED_space_api.hh"

#include "GPU_immediate.hh"
#include "GPU_matrix.hh"
#include "GPU_state.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "view3d_intern.hh"

/** \name Light Shadow Positioning Modal Operator
 * \{ */

/* Forward declaration. */
static void light_shadow_cleanup(bContext *C, wmOperator *op);

enum eShadowPositioningStage {
  SHADOW_STAGE_TARGET = 0,  /* First click: place target */
  SHADOW_STAGE_LIGHT = 1,   /* Second stage: position light */
};

struct LightShadowData {
  /** Light object being positioned. */
  Object *light_ob;
  /** Target location (3D position for shadow calculations). */
  float target_location[3];
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
  /** Current stage of operation. */
  eShadowPositioningStage stage;
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

static void light_shadow_update_position(bContext *C, LightShadowData *lsd)
{
  if (!lsd->light_ob || lsd->light_ob->type != OB_LAMP) {
    return;
  }

  if (lsd->z_adjust_mode) {
    /* Z-axis adjustment mode: move light along its local Z-axis using left/right mouse movement */
    float mouse_delta_x = float(lsd->mval[0] - lsd->z_adjust_initial_mval_x);
    float z_offset = mouse_delta_x * 0.05f; /* Scale factor for sensitivity */
    
    /* Position light at initial position + Z offset along stored local Z direction */
    float final_location[3];
    madd_v3_v3v3fl(final_location, lsd->z_adjust_initial_light_pos, lsd->light_local_z, z_offset);
    copy_v3_v3(lsd->light_ob->loc, final_location);
  }
  else {
    /* Normal shadow positioning mode */
    float ray_start[3], ray_normal[3];
    float mval_fl[2] = {float(lsd->mval[0]), float(lsd->mval[1])};
    
    ED_view3d_win_to_ray(lsd->region, mval_fl, ray_start, ray_normal);

    /* Perform raycast using snap system */
    blender::ed::transform::SnapObjectParams snap_params = {};
    snap_params.snap_target_select = SCE_SNAP_TARGET_ALL;

    float hit_location[3], hit_normal[3];
    bool hit = blender::ed::transform::snap_object_project_ray(lsd->snap_context,
                                                              lsd->depsgraph,
                                                              lsd->v3d,
                                                              &snap_params,
                                                              ray_start,
                                                              ray_normal,
                                                              nullptr,
                                                              hit_location,
                                                              hit_normal);

    if (hit) {
      if (lsd->stage == SHADOW_STAGE_LIGHT) {
        /* Stage 2: Position light based on shadow target */
        
        /* Calculate direction from shadow location to target */
        float direction_to_target[3];
        sub_v3_v3v3(direction_to_target, lsd->target_location, hit_location);
        normalize_v3(direction_to_target);
        
        /* Place light at target location with offset in calculated direction */
        float distance = lsd->offset_distance;
        float final_location[3];
        madd_v3_v3v3fl(final_location, lsd->target_location, direction_to_target, distance);
        copy_v3_v3(lsd->light_ob->loc, final_location);
        
        /* Orient light toward hit location (shadow target) */
        float direction_to_hit[3];
        sub_v3_v3v3(direction_to_hit, hit_location, final_location);
        normalize_v3(direction_to_hit);
        
        /* Use negative Z axis for light forward direction */
        float neg_z_axis[3] = {0.0f, 0.0f, -1.0f};
        float quat[4];
        rotation_between_vecs_to_quat(quat, neg_z_axis, direction_to_hit);
        quat_to_eul(lsd->light_ob->rot, quat);
      }
    }
  }

  /* Tag for update */
  DEG_id_tag_update(&lsd->light_ob->id, ID_RECALC_TRANSFORM);
  WM_event_add_notifier(C, NC_OBJECT | ND_TRANSFORM, lsd->light_ob);
}


static wmOperatorStatus light_shadow_positioning_invoke(bContext *C,
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
  LightShadowData *lsd = static_cast<LightShadowData *>(MEM_callocN(sizeof(LightShadowData), __func__));
  
  /* Initialize modal data. */
  lsd->light_ob = ob;
  zero_v3(lsd->target_location);
  lsd->mval[0] = event->mval[0];
  lsd->mval[1] = event->mval[1];
  lsd->region = CTX_wm_region(C);
  lsd->rv3d = CTX_wm_region_view3d(C);
  lsd->v3d = CTX_wm_view3d(C);
  lsd->depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  lsd->stage = SHADOW_STAGE_TARGET;
  lsd->z_adjust_mode = false;
  lsd->offset_distance = 10.0f;  /* Default shadow distance */

  /* Create snap context. */
  lsd->snap_context = blender::ed::transform::snap_object_context_create(CTX_data_scene(C), 0);

  /* Add timer. */
  wmWindowManager *wm = CTX_wm_manager(C);
  lsd->timer = WM_event_timer_add(wm, CTX_wm_window(C), TIMER, 0.02f);

  /* Set custom data. */
  op->customdata = lsd;

  /* Set modal cursor. */
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_CROSS);

  /* Add modal handler. */
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus light_shadow_positioning_modal(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent *event)
{
  LightShadowData *lsd = static_cast<LightShadowData *>(op->customdata);

  switch (event->type) {
    case MOUSEMOVE:
      lsd->mval[0] = event->mval[0];
      lsd->mval[1] = event->mval[1];
      if (lsd->stage == SHADOW_STAGE_LIGHT) {
        light_shadow_update_position(C, lsd);
      }
      break;
    
    case LEFTMOUSE:
      if (event->val == KM_PRESS) {
        if (lsd->stage == SHADOW_STAGE_TARGET) {
          /* Stage 1: Set target location from raycast */
          float ray_start[3], ray_normal[3];
          float mval_fl[2] = {float(lsd->mval[0]), float(lsd->mval[1])};
          ED_view3d_win_to_ray(lsd->region, mval_fl, ray_start, ray_normal);

          blender::ed::transform::SnapObjectParams snap_params = {};
          snap_params.snap_target_select = SCE_SNAP_TARGET_ALL;

          float hit_location[3], hit_normal[3];
          bool hit = blender::ed::transform::snap_object_project_ray(lsd->snap_context,
                                                                    lsd->depsgraph,
                                                                    lsd->v3d,
                                                                    &snap_params,
                                                                    ray_start,
                                                                    ray_normal,
                                                                    nullptr,
                                                                    hit_location,
                                                                    hit_normal);

          if (hit) {
            /* Store target location in memory only */
            copy_v3_v3(lsd->target_location, hit_location);
            
            /* Move to stage 2 */
            lsd->stage = SHADOW_STAGE_LIGHT;
          }
        } else {
          /* Stage 2: Finish positioning */
          light_shadow_cleanup(C, op);
          return OPERATOR_FINISHED;
        }
      }
      break;
    
    case EVT_ZKEY:
      if (event->val == KM_PRESS && lsd->stage == SHADOW_STAGE_LIGHT) {
        /* Start Z-axis adjustment mode (only available in stage 2) */
        lsd->z_adjust_mode = true;
        lsd->z_adjust_initial_mval_x = lsd->mval[0];
        copy_v3_v3(lsd->z_adjust_initial_light_pos, lsd->light_ob->loc);
        
        /* Calculate light's local Z axis from its current rotation */
        float light_matrix[4][4];
        loc_eul_size_to_mat4(light_matrix, 
                             lsd->light_ob->loc, 
                             lsd->light_ob->rot, 
                             lsd->light_ob->scale);
        
        /* Extract local Z axis (negative Z for light forward direction) */
        lsd->light_local_z[0] = -light_matrix[2][0];
        lsd->light_local_z[1] = -light_matrix[2][1];
        lsd->light_local_z[2] = -light_matrix[2][2];
      }
      else if (event->val == KM_RELEASE && lsd->z_adjust_mode) {
        /* End Z-axis adjustment mode and update offset distance */
        lsd->z_adjust_mode = false;
        
        /* Calculate new offset distance from current position to target */
        float distance_vec[3];
        sub_v3_v3v3(distance_vec, lsd->light_ob->loc, lsd->target_location);
        lsd->offset_distance = len_v3(distance_vec);
        
        /* Resume normal positioning */
        light_shadow_update_position(C, lsd);
      }
      break;

    case EVT_ESCKEY:
      light_shadow_cleanup(C, op);
      return OPERATOR_CANCELLED;
      
    case MIDDLEMOUSE:
    case WHEELUPMOUSE:
    case WHEELDOWNMOUSE:
      return OPERATOR_PASS_THROUGH;

    case TIMER:
      /* Update position periodically if in light positioning stage. */
      if (lsd->stage == SHADOW_STAGE_LIGHT) {
        light_shadow_update_position(C, lsd);
      }
      break;

    default:
      /* Ignore other events. */
      break;
  }

  return OPERATOR_RUNNING_MODAL;
}

static void light_shadow_cleanup(bContext *C, wmOperator *op)
{
  LightShadowData *lsd = static_cast<LightShadowData *>(op->customdata);
  
  if (lsd) {
    /* Clean up resources. */
    if (lsd->snap_context) {
      blender::ed::transform::snap_object_context_destroy(lsd->snap_context);
    }
    if (lsd->timer) {
      wmWindowManager *wm = CTX_wm_manager(C);
      WM_event_timer_remove(wm, CTX_wm_window(C), lsd->timer);
    }
    
    MEM_freeN(lsd);
    op->customdata = nullptr;
  }

  /* Restore cursor. */
  WM_cursor_modal_restore(CTX_wm_window(C));
}

static void light_shadow_positioning_cancel(bContext *C, wmOperator *op)
{
  light_shadow_cleanup(C, op);
}

void VIEW3D_OT_light_shadow_positioning(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Light Shadow Positioning";
  ot->description = "Interactively position light for shadow casting (two-step process)";
  ot->idname = "VIEW3D_OT_light_shadow_positioning";

  /* api callbacks */
  ot->invoke = light_shadow_positioning_invoke;
  ot->modal = light_shadow_positioning_modal;
  ot->cancel = light_shadow_positioning_cancel;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/** \} */