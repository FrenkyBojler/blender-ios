#include "draw_context_private.hh"

#include "DNA_object_types.h"
#include "BKE_object.hh"

#include "DNA_layer_types.h"

#include "BLI_listbase.h"

#include "BLI_math_vector.hh"
#include "BLI_math_matrix.hh"

#include "CLG_log.h"

#include "draw_handle.hh"
#include "DEG_depsgraph_query.hh"


using namespace blender::math;

CLG_LogRef LOG_DRAW_LOD = {"draw.lod"};

namespace blender::draw {

/* -------------------------------------------------------------------- */
/** \name LOD Selection (Draw-only)
 * \{ */




// Object *DRW_object_lod_select(const ObjectRef &ref,
//                               const DRWContext &draw_ctx)
// {
//   fprintf(stderr, "\n[LOD] --- DRW_object_lod_select ENTER ---\n");

//   Object *eval_ob = ref.object;
//   Object *base_ob = DEG_get_original(eval_ob);

//   fprintf(stderr,
//           "[LOD] eval_ob=%s (%p) evaluated=%d\n",
//           eval_ob->id.name + 2,
//           (void *)eval_ob,
//           DEG_is_evaluated(&eval_ob->id));

//   fprintf(stderr,
//           "[LOD] base_ob=%s (%p)\n",
//           base_ob->id.name + 2,
//           (void *)base_ob);

//   /* ---------------------------------------------------- */
//   /* No LODs                                              */
//   /* ---------------------------------------------------- */

//   if (BLI_listbase_is_empty(&base_ob->lod_items)) {
//     fprintf(stderr, "[LOD] no lod_items -> return eval_ob\n");
//     return eval_ob;
//   }

//   fprintf(stderr, "[LOD] lod_items present\n");

//   /* ---------------------------------------------------- */
//   /* View position                                        */
//   /* ---------------------------------------------------- */

//   float3 view_pos;
//   bool have_view_pos = false;

//   if (draw_ctx.rv3d) {
//     view_pos = float3(draw_ctx.rv3d->viewinv[3]);
//     have_view_pos = true;

//     fprintf(stderr,
//             "[LOD] view source: RV3D pos=(%.3f %.3f %.3f)\n",
//             view_pos.x,
//             view_pos.y,
//             view_pos.z);
//   }
//   else if (draw_ctx.scene && draw_ctx.scene->camera) {
//     view_pos = draw_ctx.scene->camera->object_to_world().location();
//     have_view_pos = true;

//     fprintf(stderr,
//             "[LOD] view source: scene camera %s\n",
//             draw_ctx.scene->camera->id.name + 2);
//   }
//   else {
//     fprintf(stderr, "[LOD] NO VIEW SOURCE\n");
//   }

//   if (!have_view_pos) {
//     fprintf(stderr, "[LOD] no view position -> return eval_ob\n");
//     return eval_ob;
//   }

//   /* ---------------------------------------------------- */
//   /* Distance (evaluated transform)                       */
//   /* ---------------------------------------------------- */

//   const float3 ob_pos = eval_ob->object_to_world().location();
//   const float dist = math::distance(view_pos, ob_pos);

//   fprintf(stderr,
//           "[LOD] object pos=(%.3f %.3f %.3f) dist=%.3f\n",
//           ob_pos.x,
//           ob_pos.y,
//           ob_pos.z,
//           dist);

//   /* ---------------------------------------------------- */
//   /* LOD selection                                        */
//   /* ---------------------------------------------------- */

//   Object *selected_eval = eval_ob;
//   int lod_index = 0;

//   LISTBASE_FOREACH (Lod *, lod, &base_ob->lod_items) {

//     fprintf(stderr,
//             "[LOD] --- LOD[%d] ---\n",
//             lod_index);

//     if (!lod->target) {
//       fprintf(stderr,
//               "[LOD] LOD[%d] target=NULL (skipping)\n",
//               lod_index);
//       lod_index++;
//       continue;
//     }

//     fprintf(stderr,
//             "[LOD] LOD[%d] target=%s (%p)\n",
//             lod_index,
//             lod->target->id.name + 2,
//             (void *)lod->target);

//     const ID *lod_eval_id =
//         DEG_get_evaluated_id(draw_ctx.depsgraph, &lod->target->id);

//     if (!lod_eval_id) {
//       fprintf(stderr,
//               "[LOD] LOD[%d] target has NO evaluated ID\n",
//               lod_index);
//       lod_index++;
//       continue;
//     }

//     Object *lod_eval = (Object *)lod_eval_id;

//     fprintf(stderr,
//             "[LOD] LOD[%d] evaluated target=%s (%p)\n",
//             lod_index,
//             lod_eval->id.name + 2,
//             (void *)lod_eval);

//     fprintf(stderr,
//             "[LOD] LOD[%d] test: dist=%.3f threshold=%.3f\n",
//             lod_index,
//             dist,
//             lod->distance);

//     if (dist >= lod->distance) {
//       selected_eval = lod_eval;
//       fprintf(stderr,
//               "[LOD] LOD[%d] SELECTED\n",
//               lod_index);
//     }
//     else {
//       fprintf(stderr,
//               "[LOD] LOD[%d] not selected\n",
//               lod_index);
//     }

//     lod_index++;
//   }

//   /* ---------------------------------------------------- */
//   /* Result                                               */
//   /* ---------------------------------------------------- */

//   if (selected_eval != eval_ob) {
//     fprintf(stderr,
//             "[LOD] RESULT: %s -> %s\n",
//             eval_ob->id.name + 2,
//             selected_eval->id.name + 2);
//   }
//   else {
//     fprintf(stderr,
//             "[LOD] RESULT: base object retained\n");
//   }

//   fprintf(stderr, "[LOD] --- DRW_object_lod_select EXIT ---\n");

//   return selected_eval;
// }







Object *DRW_object_lod_select(const ObjectRef &ref,
                              const DRWContext &draw_ctx)
{
  Object *eval_ob = ref.object;
  Object *base_ob = DEG_get_original(eval_ob);

  CLOG_INFO(&LOG_DRAW_LOD,
            "LOD select: base=%s eval=%d",
            base_ob->id.name + 2,
            DEG_is_evaluated(&eval_ob->id));

  /* No LODs → identity draw */
  if (BLI_listbase_is_empty(&base_ob->lod_items)) {
    CLOG_INFO(&LOG_DRAW_LOD, "  no lod_items → using base");
    return eval_ob;
  }

  /* ---------------------------------------------------- */
  /* View position                                        */
  /* ---------------------------------------------------- */

  float3 view_pos;
  bool have_view_pos = false;

  if (draw_ctx.rv3d) {
    view_pos = float3(draw_ctx.rv3d->viewinv[3]);
    have_view_pos = true;
    CLOG_INFO(&LOG_DRAW_LOD,
              "  view source: RV3D (%.2f %.2f %.2f)",
              view_pos.x,
              view_pos.y,
              view_pos.z);
  }
  else if (draw_ctx.scene && draw_ctx.scene->camera) {
    view_pos = draw_ctx.scene->camera->object_to_world().location();
    have_view_pos = true;
    CLOG_INFO(&LOG_DRAW_LOD,
              "  view source: scene camera %s",
              draw_ctx.scene->camera->id.name + 2);
  }

  if (!have_view_pos) {
    CLOG_WARN(&LOG_DRAW_LOD, "  no view position → fallback to base");
    return eval_ob;
  }

  /* ---------------------------------------------------- */
  /* Distance (evaluated transform, instance-safe)        */
  /* ---------------------------------------------------- */

  const float3 ob_pos = eval_ob->object_to_world().location();
  const float dist = math::distance(view_pos, ob_pos);

  CLOG_INFO(&LOG_DRAW_LOD,
            "  object pos=(%.2f %.2f %.2f) dist=%.3f",
            ob_pos.x,
            ob_pos.y,
            ob_pos.z,
            dist);

  /* ---------------------------------------------------- */
  /* LOD selection (original metadata → evaluated object) */
  /* ---------------------------------------------------- */

  Object *selected_eval = eval_ob;

  // TODO(Tri): Important: transform correctness: Don't draw lod at its original transform, but at the object's location...

  LISTBASE_FOREACH (Lod *, lod, &base_ob->lod_items) {
    if (!lod->target) {
      CLOG_WARN(&LOG_DRAW_LOD, "  LOD entry with null target");
      continue;
    }

    const ID *lod_eval_id = DEG_get_evaluated_id(
        draw_ctx.depsgraph, &lod->target->id);

    if (!lod_eval_id) {
      CLOG_WARN(&LOG_DRAW_LOD,
                "  LOD target %s has no evaluated ID",
                lod->target->id.name + 2);
      continue;
    }

    Object *lod_eval = (Object *)lod_eval_id;

    CLOG_INFO(&LOG_DRAW_LOD,
              "  test LOD: target=%s dist=%.3f threshold=%.3f",
              lod_eval->id.name + 2,
              dist,
              lod->distance);

    if (dist >= lod->distance) {
      selected_eval = lod_eval;
      CLOG_INFO(&LOG_DRAW_LOD,
                "    → selecting %s",
                selected_eval->id.name + 2);
    }
    else {
      CLOG_INFO(&LOG_DRAW_LOD, "    → break");
      // break;
    }
  }

  if (selected_eval != eval_ob) {
    CLOG_INFO(&LOG_DRAW_LOD,
              "LOD RESULT: %s → %s",
              base_ob->id.name + 2,
              selected_eval->id.name + 2);
  }
  else {
    CLOG_INFO(&LOG_DRAW_LOD, "LOD RESULT: base object");
  }

  return selected_eval;
}








// Object *DRW_object_lod_select(const Object *ob,
//                               const DRWContext *draw_ctx)
// {
//   if (BLI_listbase_is_empty(&ob->lod_items)) {
//     return const_cast<Object *>(ob);
//   }

//   if (!draw_ctx || !draw_ctx->rv3d) {
//     return const_cast<Object *>(ob);
//   }

//   const float3 cam_pos(draw_ctx->rv3d->viewinv[3]);
//   const float3 ob_pos(ob->object_to_world().location());
//   const float dist = math::distance(cam_pos, ob_pos);

//   Object *best = const_cast<Object *>(ob);

//   LISTBASE_FOREACH (Lod *, lod, &ob->lod_items) {
//     Object *lod_ob = lod->target;
//     if (!lod_ob) {
//       continue;
//     }

//     if (dist >= lod->distance) {
//       best = lod_ob;
//     }
//   }

//   return best;
// }











// DRWLODResult DRW_object_lod_select(const Object *ob,
//                                   const DRWContext *draw_ctx)
// {
//   DRWLODResult result{nullptr, nullptr};

//   if (BLI_listbase_is_empty(&ob->lod_items)) {
//     return result;
//   }

//   if (!draw_ctx || !draw_ctx->rv3d) {
//     return result;
//   }

//   const float3 cam_pos(draw_ctx->rv3d->viewinv[3]);
//   const float3 ob_pos(ob->object_to_world().location());
//   const float dist = math::distance(cam_pos, ob_pos);

//   LISTBASE_FOREACH (Lod *, lod, &ob->lod_items) {
//     if (lod->target == nullptr) {
//       continue;
//     }

//     Object *lod_eval = DEG_get_evaluated_object(draw_ctx->depsgraph,
//                                                 lod->target);
//     if (!lod_eval || !lod_eval->runtime) {
//       continue;
//     }

//     ID *data_eval = lod_eval->runtime->data_eval;
//     if (!data_eval || GS(data_eval->name) != ID_ME) {
//       continue;
//     }

//     if (dist >= lod->distance) {
//       result.data_eval = data_eval;
//       result.geometry_set = lod_eval->runtime->geometry_set_eval;
//     }
//   }

//   return result;
// }



// !!!(Tri): Eliminates convenience of lods... 
static bool lod_is_renderable(const Object *ob)
{
  return (ob->base_flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) != 0;
}


// TODO: Better idea to rename this and add a similar helper which returns geometry instead of Object...
// Object *DRW_object_lod_select(const Object *ob,
//                               const DRWContext *draw_ctx)
// {
//   if (BLI_listbase_is_empty(&ob->lod_items)) {
//     return const_cast<Object *>(ob);
//   }

//   if (!draw_ctx || !draw_ctx->rv3d) {
//     CLOG_INFO(&LOG_DRAW_LOD, "No RV3D for object %s", ob->id.name + 2);
//     return const_cast<Object *>(ob);
//   }

//   /* --- IMPORTANT FIX HERE --- */

//   /* rv3d->viewinv is float[4][4] */
//   const blender::float3 cam_pos = blender::float3(draw_ctx->rv3d->viewinv[3]);

//   /* object_to_world().location() is float4, take xyz */
//   const blender::float3 ob_pos =
//       blender::float3(ob->object_to_world().location());

//   const float dist = blender::math::distance(cam_pos, ob_pos);

//   Object *best = const_cast<Object *>(ob);

//   LISTBASE_FOREACH (Lod *, lod, &ob->lod_items) {
//     if (lod->target && dist >= lod->distance && lod_is_renderable(lod->target)) { //new
//       best = lod->target;
//     }
//   }

//   CLOG_INFO(&LOG_DRAW_LOD,
//             "LOD select: %s -> %s (dist %.2f)",
//             ob->id.name + 2,
//             best->id.name + 2,
//             dist);

//   return best;
// }

} // namespace blender::draw

/** \} */
