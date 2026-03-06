/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Virtual shadow-mapping: Update tagging
 *
 * Any updated shadow caster needs to tag the shadow map tiles it was in and is now into.
 * This is done in 2 pass of this same shader. One for past object bounds and one for new object
 * bounds. The bounding boxes are rasterized and each fragment shader invocation tags the
 * appropriate tiles.
 */

#pragma once
#pragma create_info

#include "eevee_defines.hh"
#include "eevee_shadow_shared.hh"

#include "eevee_shadow_tilemap_lib.glsl"

namespace eevee::shadow {

struct TagUpdate {
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo draw_view_culling;

  [[push_constant]] int tilemap_count;

  [[storage(0, read_write)]] ShadowTileMapData (&tilemaps_buf)[];
  [[storage(1, read_write)]] uint (&tiles_buf)[];
  [[storage(5, read)]] const ObjectBounds (&bounds_buf)[];
  [[storage(6, read)]] const uint (&resource_ids_buf)[];
};

struct VertIn {
  [[attribute(0)]] float3 pos;
};

struct VertOut {
  [[flat]] int tilemap_index;
};

[[vertex]]
void tag_update_vert([[resource_table]] TagUpdate &srt,
                     [[instance_id]] const int inst_id,
                     [[in]] const VertIn &v_in,
                     [[out]] VertOut &v_out,
                     [[position]] float4 &out_position)
{
  v_out.tilemap_index = inst_id % srt.tilemap_count;
  uint resource_id = srt.resource_ids_buf[inst_id] & 0x7FFFFFFFu;

  ObjectBounds bounds = srt.bounds_buf[resource_id];
  if (!drw_bounds_are_valid(bounds)) {
    out_position = float4(1.0f);
    return;
  }

  ShadowTileMapData tilemap = srt.tilemaps_buf[v_out.tilemap_index];

  /* Convert from -1..1 box shape to 0..1 box. */
  float3 lP = max(float3(0), v_in.pos);

  float3 P = lP.x * bounds.bounding_corners[1].xyz + lP.y * bounds.bounding_corners[2].xyz +
             lP.z * bounds.bounding_corners[3].xyz + bounds.bounding_corners[0].xyz;

  out_position = tilemap.winmat * (tilemap.viewmat * float4(P, 1.0f));
}

[[fragment]]
void tag_update_frag([[resource_table]] TagUpdate &srt,
                     [[frag_coord]] const float4 frag_coord,
                     [[in]] const VertOut &v_out)
{
  ShadowTileMapData tilemap = srt.tilemaps_buf[v_out.tilemap_index];

  uint2 texel = uint2(frag_coord.xy);
  for (int lod = 0; lod <= SHADOW_TILEMAP_LOD; lod++) {
    /* NOTE: Can't reject any thread in LODs because we need to be conservative.
     * This can create some atomic contention but for now we live with that. */
    int tile_index = shadow_tile_offset(texel >> lod, tilemap.tiles_index, lod);
    atomicOr(srt.tiles_buf[tile_index], uint(SHADOW_DO_UPDATE));
  }
}

PipelineGraphic tag_update(tag_update_vert, tag_update_frag);

}  // namespace eevee::shadow
