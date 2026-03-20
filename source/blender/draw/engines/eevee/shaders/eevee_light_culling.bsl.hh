/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "eevee_light_shared.hh"
#include "eevee_shadow_shared.hh"

namespace eevee::light {

struct Resources {
  [[legacy_info]] ShaderCreateInfo eevee_sampling_data;
  [[legacy_info]] ShaderCreateInfo eevee_global_ubo;
  [[storage(0, read)]] const LightCullingData &light_cull_buf;
  [[storage(1, read_write)]] LightData (&light_buf)[];
  [[storage(2, read_write)]] ShadowTileMapData (&tilemaps_buf)[];
  [[storage(3, read_write)]] ShadowTileMapClip (&tilemaps_clip_buf)[];
};

[[compute, local_size(CULLING_SELECT_GROUP_SIZE)]]

void shadow_setup_main([[resource_table]] Resources &srt,
                       [[global_invocation_id]] const uint3 global_id,
                       [[local_invocation_id]] const uint3 local_id,
                       [[local_invocation_index]] const uint3 local_index)
{
}

PipelineCompute shadow_setup(shadow_setup_main);

}  // namespace eevee::light
