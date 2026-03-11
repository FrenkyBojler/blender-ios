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
#include "gpu_shader_math_matrix_transform_lib.glsl"

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
  [[flat]] uint tilemap_index;
};

[[vertex]]
void tag_update_vert([[resource_table]] TagUpdate &srt,
                     [[instance_id]] const int inst_per_tilemap_id,
                     [[in]] const VertIn &v_in,
                     [[out]] VertOut &v_out,
                     [[position]] float4 &out_position)
{
  v_out.tilemap_index = uint(inst_per_tilemap_id) % uint(srt.tilemap_count);
  const uint inst_id = uint(inst_per_tilemap_id) / uint(srt.tilemap_count);
  const uint resource_id = srt.resource_ids_buf[inst_id] & 0x7FFFFFFFu;

  ObjectBounds bounds = srt.bounds_buf[resource_id];
  if (!drw_bounds_are_valid(bounds)) {
    out_position = float4(1.0f);
    return;
  }

  ShadowTileMapData tilemap = srt.tilemaps_buf[v_out.tilemap_index];

  const float3 ls_N = v_in.pos;
  const /* Convert from -1..1 box shape to 0..1 box. */
      float3 ls_P = max(float3(0), v_in.pos);

  const float3 P = ls_P.x * bounds.bounding_corners[1].xyz +
                   ls_P.y * bounds.bounding_corners[2].xyz +
                   ls_P.z * bounds.bounding_corners[3].xyz + bounds.bounding_corners[0].xyz;

  const float4 hs_P = tilemap.winmat * (tilemap.viewmat * float4(P, 1.0f));
  /* Clip space normals are the same direction as the viewspace one since the projection has aspect
   * ratio of 1:1. */
  const float3 hs_N = transform_direction(tilemap.viewmat, ls_N);

  out_position = hs_P;

  /* To emulate conservative rasterization, we inflate the bounding box by 1 pixel. */
  const float2 ndc_pixel_size = 2.0f / float2(SHADOW_TILEMAP_RES);
  out_position.xy += sign(hs_N.xy) * ndc_pixel_size * out_position.w;

  const bool is_persp = tilemap.winmat[3][3] == 0.0f;
  /* Flatten the box to avoid loosing pixel when the box extend beyond the far clip plane. */
  if (!is_persp || (out_position.z > out_position.w && out_position.w > 0.0)) {
    out_position.z = out_position.w - 1e-16;
  }
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
