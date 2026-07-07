/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "overlay_mode_transfer.hh"

#include "BLI_math_color_c.hh"

#include "ED_object.hh"

#include "draw_cache.hh"
#include "draw_sculpt.hh"

namespace blender::draw::overlay {

void ModeTransfer::begin_sync(Resources &res, const State &state)
{
  object_factors_ = ed::object::mode_transfer_overlay_current_state();

  enabled_ = state.is_space_v3d() && !res.is_selection() && !object_factors_.is_empty();

  if (!enabled_) {
    /* Not used. But release the data. */
    ps_.init();
    return;
  }

  ui::theme::get_color_3fv(TH_VERTEX_SELECT, flash_color_);
  srgb_to_linearrgb_v4(flash_color_, flash_color_);

  ps_.init();
  ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_DEPTH_LESS_EQUAL | DRW_STATE_WRITE_DEPTH,
                state.clipping_plane_count);
  ps_.shader_set(res.shaders->uniform_color.get());
  ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
  ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
}

void ModeTransfer::object_sync(Manager &manager,
                               const ObjectRef &ob_ref,
                               Resources & /*res*/,
                               const State &state)
{
  if (!enabled_) {
    return;
  }

  const std::optional<float> alpha_opt = object_factors_.lookup_try_as(ob_ref.object->id.name);
  if (!alpha_opt) {
    return;
  }

  const bool renderable = DRW_object_is_renderable(ob_ref.object);
  const bool draw_surface = (ob_ref.object->dt >= OB_WIRE) &&
                            (renderable || (ob_ref.object->dt == OB_WIRE));
  if (!draw_surface) {
    return;
  }

  constexpr float flash_alpha = 0.25f;
  const float alpha = *alpha_opt * flash_alpha;

  ps_.push_constant("ucolor", float4(flash_color_.xyz() * alpha, alpha));

  const bool use_sculpt_pbvh = BKE_sculptsession_use_pbvh_draw(ob_ref.object, state.rv3d) &&
                               !state.is_image_render;
  if (use_sculpt_pbvh) {
    ResourceHandleRange handle = manager.unique_handle_for_sculpt(ob_ref);

    for (SculptBatch &batch : sculpt_batches_get(ob_ref.object, SCULPT_BATCH_DEFAULT)) {
      ps_.draw(batch.batch, handle);
    }
  }
  else {
    gpu::Batch *geom = DRW_cache_object_surface_get(const_cast<Object *>(ob_ref.object));
    if (geom) {
      ps_.draw(geom, manager.unique_handle(ob_ref));
    }
  }
}

void ModeTransfer::draw(Framebuffer &framebuffer, Manager &manager, View &view)
{
  if (!enabled_) {
    return;
  }

  GPU_framebuffer_bind(framebuffer);
  manager.submit(ps_, view);

  /* Request redraws until the object fades out (enabled_ will be reset to false). */
  DRW_viewport_request_redraw();
}

}  // namespace blender::draw::overlay
