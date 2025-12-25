#include "draw_context_private.hh"

#include "DNA_object_types.h"
#include "BKE_object.hh"

#include "DNA_layer_types.h"

#include "BLI_listbase.h"

#include "BLI_math_vector.hh"
#include "BLI_math_matrix.hh"

#include "CLG_log.h"

using namespace blender::math;

CLG_LogRef LOG_DRAW_LOD = {"draw.lod"};

namespace blender::draw {

/* -------------------------------------------------------------------- */
/** \name LOD Selection (Draw-only)
 * \{ */


// Wrong
DRWLODResult DRW_object_lod_select_(const Object *ob,
                                    const DRWContext *draw_ctx)
{
  DRWLODResult result{nullptr};

  if (BLI_listbase_is_empty(&ob->lod_items)) {
    return result;
  }

  if (!draw_ctx || !draw_ctx->rv3d) {
    return result;
  }

  const float3 cam_pos(draw_ctx->rv3d->viewinv[3]);
  const float3 ob_pos(ob->object_to_world().location());
  const float dist = math::distance(cam_pos, ob_pos);

  LISTBASE_FOREACH (Lod *, lod, &ob->lod_items) {
    if (lod->target && dist >= lod->distance) {
      result.override_data = static_cast<ID *>(lod->target->data); // Wrong
    }
  }

  return result;
}



// !!!(Tri): Eliminates convenience of lods... 
static bool lod_is_renderable(const Object *ob)
{
  return (ob->base_flag & BASE_ENABLED_AND_MAYBE_VISIBLE_IN_VIEWPORT) != 0;
}

// TODO: Better idea to rename this and add a similar helper which returns geometry instead of Object...
Object *DRW_object_lod_select(const Object *ob,
                              const DRWContext *draw_ctx)
{
  if (BLI_listbase_is_empty(&ob->lod_items)) {
    return const_cast<Object *>(ob);
  }

  if (!draw_ctx || !draw_ctx->rv3d) {
    CLOG_INFO(&LOG_DRAW_LOD, "No RV3D for object %s", ob->id.name + 2);
    return const_cast<Object *>(ob);
  }

  /* --- IMPORTANT FIX HERE --- */

  /* rv3d->viewinv is float[4][4] */
  const blender::float3 cam_pos = blender::float3(draw_ctx->rv3d->viewinv[3]);

  /* object_to_world().location() is float4, take xyz */
  const blender::float3 ob_pos =
      blender::float3(ob->object_to_world().location());

  const float dist = blender::math::distance(cam_pos, ob_pos);

  Object *best = const_cast<Object *>(ob);

  LISTBASE_FOREACH (Lod *, lod, &ob->lod_items) {
    if (lod->target && dist >= lod->distance && lod_is_renderable(lod->target)) { //new
      best = lod->target;
    }
  }

  CLOG_INFO(&LOG_DRAW_LOD,
            "LOD select: %s -> %s (dist %.2f)",
            ob->id.name + 2,
            best->id.name + 2,
            dist);

  return best;
}



// Object *DRW_object_lod_select(const Object *ob,
//                               const DRWContext *draw_ctx)
// {
//     if (BLI_listbase_is_empty(&ob->lod_items)) {
//     return const_cast<Object *>(ob);
//   }

//   if (!draw_ctx || !draw_ctx->rv3d) {
//     CLOG_INFO(&LOG_DRAW_LOD, "No RV3D for object %s", ob->id.name + 2);
//     return const_cast<Object *>(ob);
//   }

//   const float3 cam_pos = draw_ctx->rv3d->viewinv[3];
//   const float3 ob_pos = ob->object_to_world().location();
//   const float dist = blender::math::distance(cam_pos, ob_pos);

//   Object *best = const_cast<Object *>(ob);

//   int i = 0;
//   LISTBASE_FOREACH (Lod *, lod, &ob->lod_items) {
//     if (lod->target) {
//       CLOG_INFO(&LOG_DRAW_LOD,
//                 "LOD[%d] target=%s dist=%.2f threshold=%.2f",
//                 i,
//                 lod->target->id.name + 2,
//                 dist,
//                 lod->distance);

//       if (dist >= lod->distance) {
//         best = lod->target;
//       }
//     }
//     i++;
//   }

//   CLOG_INFO(&LOG_DRAW_LOD,
//             "Selected draw object: %s (base: %s)",
//             best->id.name + 2,
//             ob->id.name + 2);

//   return best;
// }

} // namespace blender::draw

/** \} */
