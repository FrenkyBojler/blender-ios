/* SPDX-FileCopyrightText: 2019 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#include "overlay_private.hh"

#include "BKE_movieclip.hh"

#include "DNA_world_types.h"

namespace blender::draw::overlay {

float4 Resources::background_color_get(const State &state)
{
  if (state.v3d->shading.background_type == V3D_SHADING_BACKGROUND_WORLD) {
    if (state.scene->world) {
      return float4(float3(&state.scene->world->horr), 0.0f);
    }
  }
  else if (state.v3d->shading.background_type == V3D_SHADING_BACKGROUND_VIEWPORT) {
    return state.v3d->shading.background_color;
  }
  float4 color;
  ui::theme::get_color_3fv(TH_BACK, color);
  return color;
}

void Resources::free_movieclips_textures()
{
  /* Free Movie clip textures after rendering */
  for (MovieClip *clip : bg_movie_clips) {
    BKE_movieclip_free_gputexture(clip);
  }
  bg_movie_clips.clear();
}

}  // namespace blender::draw::overlay
