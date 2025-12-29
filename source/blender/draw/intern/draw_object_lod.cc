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

// TODO(Tri): Remove
CLG_LogRef LOG_DRAW_LOD = {"draw.lod"};

namespace blender::draw {

/* -------------------------------------------------------------------- */
/** \name LOD Selection (Draw-only)
 * \{ */

Object *DRW_object_lod_select(const ObjectRef &ref,
                              const DRWContext &draw_ctx)
{
  Object *eval_ob = ref.object;
  Object *base_ob = DEG_get_original(eval_ob);

  CLOG_INFO(&LOG_DRAW_LOD,
            "LOD select: base=%s eval=%d",
            base_ob->id.name + 2,
            DEG_is_evaluated(&eval_ob->id));

  /* No LODs → draw base */
  if (BLI_listbase_is_empty(&base_ob->lod_items)) {
    CLOG_INFO(&LOG_DRAW_LOD, "  no lod_items → draw base");
    return nullptr;
  }

  /* View position */
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
    CLOG_WARN(&LOG_DRAW_LOD, "  no view position → draw base");
    return nullptr;
  }

  /* Distance (evaluated transform, instance-safe) */
  const float3 ob_pos = eval_ob->object_to_world().location();
  const float dist = math::distance(view_pos, ob_pos);

  CLOG_INFO(&LOG_DRAW_LOD,
            "  object pos=(%.2f %.2f %.2f) dist=%.3f",
            ob_pos.x,
            ob_pos.y,
            ob_pos.z,
            dist);

  // LOD selection
  Object *selected_eval = nullptr;

  LISTBASE_FOREACH (Lod *, lod, &base_ob->lod_items) {
    if (!lod->target) {
      CLOG_WARN(&LOG_DRAW_LOD, "  LOD entry with null target");
      continue;
    }

    const ID *lod_eval_id = DEG_get_evaluated_id(draw_ctx.depsgraph,
                                                &lod->target->id);
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
                lod_eval->id.name + 2);
    }
    else {
      break;
    }
  }

  if (selected_eval) {
    CLOG_INFO(&LOG_DRAW_LOD,
              "LOD RESULT: %s → %s",
              base_ob->id.name + 2,
              selected_eval->id.name + 2);
  }
  else {
    CLOG_INFO(&LOG_DRAW_LOD, "LOD RESULT: base object");
  }

  return selected_eval; /* nullptr == draw base */
}

} // namespace blender::draw

/** \} */
