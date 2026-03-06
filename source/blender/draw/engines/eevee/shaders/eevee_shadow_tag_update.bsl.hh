/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Virtual shadow-mapping: Update tagging
 *
 * Any updated shadow caster needs to tag the shadow map tiles it was in and is now into.
 * This is done in 2 pass of this same shader. One for past object bounds and one for new object
 * bounds. The bounding boxes are roughly software rasterized (just a plain rectangle) in order to
 * tag the appropriate tiles.
 */

#pragma once
#pragma create_info

#include "draw_aabb_lib.glsl"
#include "draw_intersect_lib.glsl"
#include "eevee_defines.hh"
#include "eevee_shadow_shared.hh"

#include "eevee_shadow_tilemap_lib.glsl"

namespace eevee::shadow {

struct TagUpdate {
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo draw_view_culling;

  [[storage(0, read_write)]] ShadowTileMapData (&tilemaps_buf)[];
  [[storage(1, read_write)]] uint (&tiles_buf)[];
  [[storage(5, read)]] const ObjectBounds (&bounds_buf)[];
  [[storage(6, read)]] const uint (&resource_ids_buf)[];
};

float3 safe_project(float4x4 winmat, float4x4 viewmat, int &clipped, float3 v)
{
  float4 tmp = winmat * (viewmat * float4(v, 1.0f));
  /* Detect case when point is behind the camera. */
  clipped += int(tmp.w < 0.0f);
  return tmp.xyz / tmp.w;
}

[[compute]] [[local_size(1, 1, 1)]]
void tag_update_main([[resource_table]] TagUpdate &srt,
                     [[global_invocation_id]] const uint3 global_id)
{
  ShadowTileMapData tilemap = srt.tilemaps_buf[global_id.z];

  IsectPyramid frustum;
  if (tilemap.projection_type == SHADOW_PROJECTION_CUBEFACE) {
    Pyramid pyramid = shadow_tilemap_cubeface_bounds(tilemap, int2(0), int2(SHADOW_TILEMAP_RES));
    frustum = isect_pyramid_setup(pyramid);
  }

  uint resource_id = srt.resource_ids_buf[global_id.x];
  resource_id = (resource_id & 0x7FFFFFFFu);

  ObjectBounds bounds = srt.bounds_buf[resource_id];
  if (!drw_bounds_are_valid(bounds)) {
    return;
  }
  IsectBox box = isect_box_setup(bounds.bounding_corners[0].xyz,
                                 bounds.bounding_corners[1].xyz,
                                 bounds.bounding_corners[2].xyz,
                                 bounds.bounding_corners[3].xyz);

  int clipped = 0;
  /* NDC space post projection [-1..1] (unclamped). */
  AABB aabb_ndc = aabb_init_min_max();
  for (int v = 0; v < 8; v++) {
    aabb_merge(aabb_ndc, safe_project(tilemap.winmat, tilemap.viewmat, clipped, box.corners[v]));
  }

  if (tilemap.projection_type == SHADOW_PROJECTION_CUBEFACE) {
    if (clipped == 8) {
      /* All verts are behind the camera. */
      return;
    }
    if (clipped > 0) {
      /* Not all verts are behind the near clip plane. */
      if (intersect(frustum, box)) {
        /* We cannot correctly handle this case so we fall back by covering the whole view. */
        aabb_ndc.max = float3(1.0f);
        aabb_ndc.min = float3(-1.0f);
      }
      else {
        /* Still out of the frustum. Ignore. */
        return;
      }
    }
    else {
      /* None of the verts are behind the camera. The projected AABB is correct. */
    }
  }

  AABB aabb_tag;
  AABB aabb_map = shape_aabb(float3(-0.99999f), float3(0.99999f));

  /* Directional `winmat` have no correct near/far in the Z dimension at this point.
   * Do not clip in this dimension. */
  if (tilemap.projection_type != SHADOW_PROJECTION_CUBEFACE) {
    aabb_map.min.z = -FLT_MAX;
    aabb_map.max.z = FLT_MAX;
  }

  if (!aabb_clip(aabb_map, aabb_ndc, aabb_tag)) {
    return;
  }

  /* Raster the bounding rectangle of the Box projection. */
  constexpr float tilemap_half_res = float(SHADOW_TILEMAP_RES / 2);
  int2 box_min = int2(aabb_tag.min.xy * tilemap_half_res + tilemap_half_res);
  int2 box_max = int2(aabb_tag.max.xy * tilemap_half_res + tilemap_half_res);

  for (int lod = 0; lod <= SHADOW_TILEMAP_LOD; lod++, box_min >>= 1, box_max >>= 1) {
    for (int y = box_min.y; y <= box_max.y; y++) {
      for (int x = box_min.x; x <= box_max.x; x++) {
        int tile_index = shadow_tile_offset(uint2(uint(x), uint(y)), tilemap.tiles_index, lod);
        atomicOr(srt.tiles_buf[tile_index], uint(SHADOW_DO_UPDATE));
      }
    }
  }
}

PipelineCompute tag_update(tag_update_main);

}  // namespace eevee::shadow
