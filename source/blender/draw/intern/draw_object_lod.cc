#include "draw_context_private.hh"

#include "DNA_object_types.h"
#include "BKE_object.hh"

#include "DNA_layer_types.h"

#include "BLI_listbase.h"

#include "BLI_math_vector.hh"
#include "BLI_math_matrix.hh"

#include "CLG_log.h"

#include "draw_handle.hh"

using namespace blender::math;

CLG_LogRef LOG_DRAW_LOD = {"draw.lod"};

namespace blender::draw {

/* -------------------------------------------------------------------- */
/** \name LOD Selection (Draw-only)
 * \{ */


Object *DRW_object_lod_select(const ObjectRef &ref,
                              const DRWContext &draw_ctx)
{
  // Object *base_ob = ref.object;
  Object *base_ob = ref.object->runtime->object_orig;
  if (base_ob == nullptr) {
    base_ob = ref.object;
  }

  CLOG_INFO(&LOG_DRAW_LOD,
          "LOD select: base=%s eval=%d",
          base_ob->id.name + 2,
          DEG_is_evaluated_id(&base_ob->id));


  CLOG_INFO(&LOG_DRAW_LOD,
            "LOD select: base=%s (%p)",
            base_ob->id.name + 2,
            base_ob);

  /* No LODs */
  if (BLI_listbase_is_empty(&base_ob->lod_items)) {
    CLOG_INFO(&LOG_DRAW_LOD, "  no lod_items → using base");
    return base_ob;
  }

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
    return base_ob;
  }

  // const float3 ob_pos = ref.object->object_to_world().location();
  const float3 ob_pos = eval_ob->object_to_world().location();

  const float dist = math::distance(view_pos, ob_pos);

  CLOG_INFO(&LOG_DRAW_LOD,
            "  object pos=(%.2f %.2f %.2f) dist=%.3f",
            ob_pos.x,
            ob_pos.y,
            ob_pos.z,
            dist);

  Object *selected = base_ob;

  LISTBASE_FOREACH (Lod *, lod, &base_ob->lod_items) {
    if (lod->target == nullptr) {
      CLOG_WARN(&LOG_DRAW_LOD, "  LOD entry with null target");
      continue;
    }

    CLOG_INFO(&LOG_DRAW_LOD,
              "  test LOD: target=%s dist=%.3f threshold=%.3f",
              lod->target->id.name + 2,
              dist,
              lod->distance);

    if (dist >= lod->distance) {
      selected = lod->target;
      CLOG_INFO(&LOG_DRAW_LOD,
                "    → selecting %s",
                selected->id.name + 2);
    }
    else {
      CLOG_INFO(&LOG_DRAW_LOD, "    → break");
      break;
    }
  }

  if (selected != base_ob) {
    CLOG_INFO(&LOG_DRAW_LOD,
              "LOD RESULT: %s → %s",
              base_ob->id.name + 2,
              selected->id.name + 2);
  }
  else {
    CLOG_INFO(&LOG_DRAW_LOD, "LOD RESULT: base object");
  }

  return selected;
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
