/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once
#pragma create_info

#include "infos/eevee_common_infos.hh"

COMPUTE_SHADER_CREATE_INFO(draw_view)
COMPUTE_SHADER_CREATE_INFO(eevee_gbuffer_data)

#include "draw_shader_shared.hh"
#include "draw_view_lib.glsl"
#include "eevee_defines.hh"
#include "eevee_gbuffer_read_lib.glsl"
#include "eevee_reverse_z_lib.glsl"
#include "gpu_shader_shared_exponent_lib.glsl"

namespace eevee::subsurface {

/* -------------------------------------------------------------------- */
/** \name Setup
 *
 * Select tiles that have visible SSS effect and prepare the intermediate buffers for faster
 * processing.
 * \{ */

struct Setup {
  [[legacy_info]] ShaderCreateInfo draw_view;
  [[legacy_info]] ShaderCreateInfo eevee_gbuffer_data;

  [[shared]] uint &has_visible_sss;

  [[sampler(2)]] sampler2DDepth depth_tx;

  [[image(0, read, DEFERRED_RADIANCE_FORMAT)]] const uimage2D direct_light_img;
  [[image(1, read, RAYTRACE_RADIANCE_FORMAT)]] const image2D indirect_light_img;
  [[image(2, write, SUBSURFACE_OBJECT_ID_FORMAT)]] uimage2D object_id_img;
  [[image(3, write, SUBSURFACE_RADIANCE_FORMAT)]] image2D radiance_img;

  [[storage(0, write)]] uint (&convolve_tile_buf)[];
  [[storage(1, read_write)]] DispatchCommand &convolve_dispatch_buf;
};

[[compute, local_size(SUBSURFACE_GROUP_SIZE, SUBSURFACE_GROUP_SIZE)]]
void setup_main([[resource_table]] Setup &srt,
                [[global_invocation_id]] const uint3 global_thread_id,
                [[local_invocation_index]] const uint local_thread_index)
{
  int2 texel = int2(global_thread_id.xy);

  if (local_thread_index == 0u) {
    srt.has_visible_sss = 0u;
  }

  barrier();

  const gbuffer::Layers gbuf = gbuffer::read_layers(texel);

  ClosureUndetermined cl = gbuf.layer[0];

  if (cl.type == CLOSURE_BSSRDF_BURLEY_ID) {
    float3 radiance = rgb9e5_decode(imageLoadFast(srt.direct_light_img, texel).r);
    radiance += imageLoadFast(srt.indirect_light_img, texel).rgb;

    ClosureSubsurface closure = to_closure_subsurface(cl);
    float max_radius = reduce_max(closure.sss_radius);

    uint object_id = gbuffer::read_object_id(texel);

    imageStoreFast(srt.radiance_img, texel, float4(radiance, 0.0f));
    imageStoreFast(srt.object_id_img, texel, uint4(object_id));

    float depth = reverse_z::read(texelFetch(srt.depth_tx, texel, 0).r);
    /* TODO(fclem): Check if this simplifies. */
    float vPz = drw_depth_screen_to_view(depth);
    float homogenous_coord = drw_view().winmat[2][3] * vPz + drw_view().winmat[3][3];
    float sample_scale = drw_view().winmat[0][0] * (0.5f * max_radius / homogenous_coord);
    float pixel_footprint = sample_scale * float(textureSize(gbuf_header_tx, 0).x);
    if (pixel_footprint > 1.0f) {
      /* Race condition doesn't matter here. */
      srt.has_visible_sss = 1u;
    }
  }
  else {
    /* No need to write radiance_img since the radiance won't be used at all. */
    imageStore(srt.object_id_img, texel, uint4(0));
  }

  barrier();

  if (local_thread_index == 0u) {
    if (srt.has_visible_sss > 0u) {
      uint tile_id = atomicAdd(srt.convolve_dispatch_buf.num_groups_x, 1u);
      srt.convolve_tile_buf[tile_id] = packUvec2x16(gl_WorkGroupID.xy);
      /* Race condition doesn't matter here. */
      srt.convolve_dispatch_buf.num_groups_y = 1;
      srt.convolve_dispatch_buf.num_groups_z = 1;
    }
  }
}

/** \} */

PipelineCompute setup(setup_main);

}  // namespace eevee::subsurface
