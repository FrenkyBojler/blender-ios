#include "draw_context_private.hh"

#include "DNA_object_types.h"
#include "BKE_object.hh"

#include "DNA_layer_types.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"

#include "BLI_math_vector.hh"
#include "BLI_math_matrix.hh"

#include "CLG_log.h"

#include "draw_handle.hh"
#include "DRW_render.hh"
#include "DEG_depsgraph_query.hh"


using namespace blender::math;

// TODO(Tri): Remove
CLG_LogRef LOG_DRAW_LOD = {"draw.lod"};

namespace blender::draw {

/* -------------------------------------------------------------------- */
/** \name LOD Selection (Draw-only)
 * \{ */

// TODO(Tri): clear the `lod_state_map`
Object *DRW_object_lod_select(const ObjectRef &ref,
                              const DRWContext &draw_ctx,
                              const DupliObject *dupli /* nullable */)
{
  Object *eval_ob = ref.object;
  Object *base_ob = DEG_get_original(eval_ob);

  /* No LODs → draw base */
  if (BLI_listbase_is_empty(&base_ob->lod_items)) {
    return nullptr;
  }

  /* View position */
  float3 view_pos;
  bool have_view_pos = false;

  if (draw_ctx.rv3d) {
    view_pos = float3(draw_ctx.rv3d->viewinv[3]);
    have_view_pos = true;
  }
  else if (draw_ctx.scene && draw_ctx.scene->camera) {
    view_pos = draw_ctx.scene->camera->object_to_world().location();
    have_view_pos = true;
  }

  if (!have_view_pos) {
    return nullptr;
  }

  /* Object position + instance-safe key */
  float3 ob_pos;
  uint64_t lod_key;

  if (dupli) {
    ob_pos = float3(dupli->mat[3]);
    lod_key = BLI_hash_int_2d(
        dupli->persistent_id[0],
        dupli->persistent_id[1]);
  }
  else {
    ob_pos = eval_ob->object_to_world().location();
    lod_key = eval_ob->id.session_uid;
  }

  const float dist = math::distance(view_pos, ob_pos);

  /* Per-instance draw-state */
  DRWLodState &state =
      draw_ctx.lod_state_map.lookup_or_add(lod_key, {0, FLT_MAX});

  const int last_lod = state.last_lod_index;

  Object *selected_eval = nullptr;
  int selected_lod_index = -1;

  int lod_index = 0;
  LISTBASE_FOREACH (Lod *, lod, &base_ob->lod_items) {
    if (!lod->target) {
      lod_index++;
      continue;
    }

    const ID *lod_eval_id =
        DEG_get_evaluated_id(draw_ctx.depsgraph, &lod->target->id);
    if (!lod_eval_id) {
      lod_index++;
      continue;
    }

    Object *lod_eval = (Object *)lod_eval_id;

    // TODO(Tri): Make the band adjustable (Adjustable hysteresis)
    const float hysteresis = lod->distance * 0.1f;
    const float switch_down_dist = lod->distance - hysteresis;

    if (dist >= lod->distance) {
      selected_eval = lod_eval;
      selected_lod_index = lod_index;
    }
    else if (lod_index == last_lod && dist >= switch_down_dist) {
      selected_eval = lod_eval;
      selected_lod_index = lod_index;
    }
    else {
      break;
    }

    lod_index++;
  }

  /* Update draw-state */
  state.last_lod_index = max_ii(selected_lod_index, 0);
  state.last_distance = dist;

  return selected_eval; /* nullptr == draw base */
}

} // namespace blender::draw

/** \} */
