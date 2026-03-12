/* SPDX-FileCopyrightText: 2020-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once
#pragma create_info

#include "gpu_shader_compat.hh"
#include "gpu_shader_fullscreen_lib.glsl"
#include "infos/overlay_common_infos.hh"
#include "overlay_shader_shared.hh"

SHADER_LIBRARY_CREATE_INFO(draw_globals)

namespace overlay::xray_fade {

struct Texel {
  float depth;
  float xray_depth;
};

/* Shader resource table. */
struct Resources {
  [[legacy_info]] ShaderCreateInfo draw_globals;

  [[sampler(0)]] const sampler2DDepth xray_depth_tx;
  [[sampler(1)]] const sampler2DDepth depth_tx;
  [[sampler(2)]] const sampler2DDepth depth_in_front_tx;
  [[sampler(3)]] const sampler2DDepth xray_depth_in_front_tx;

  [[push_constant]] float opacity;

  Texel sample(float2 uv)
  {
    return {
        .depth = textureLod(depth_tx, uv, 0.0f).r,
        .xray_depth = textureLod(xray_depth_tx, uv, 0.0f).r,
    };
  }

  Texel sample_in_front(float2 uv)
  {
    return {
        .depth = textureLod(depth_in_front_tx, uv, 0.0f).r,
        .xray_depth = textureLod(xray_depth_in_front_tx, uv, 0.0f).r,
    };
  }
};

/**
 * Vertex stage.
 * Outputs [0,1] UV to a fullscreen quad.
 */
struct VertexOutput {
  [[smooth]] float2 uv;
};
[[vertex]] void vert_main([[vertex_id]] const int &vert_id,
                          [[out]] VertexOutput &vert,
                          [[position]] float4 &position)
{
  fullscreen_vertex(vert_id, position, vert.uv);
}

/*
 * Fragment stage.
 * Outputs soft darkening fade on xray'd objects dependent on depth/xray-depth.
 */
struct FragmentOutput {
  [[frag_color(0)]] float4 color;
};
[[fragment]] void frag_main([[frag_coord]] const float4 &frag_coord,
                            [[resource_table]] Resources &srt,
                            [[in]] const VertexOutput &vert,
                            [[out]] FragmentOutput &frag)
{
  Texel texel_in_front = srt.sample_in_front(vert.uv);

  if (texel_in_front.xray_depth != 1.0f) {
    if (texel_in_front.depth < texel_in_front.xray_depth) {
      frag.color = float4(srt.opacity);
      return;
    }

    gpu_discard_fragment();
    return;
  }

  Texel texel = srt.sample_in_front(vert.uv);

  /* Merge infront depth. */
  if (texel_in_front.depth != 1.0f) {
    texel.depth = 0.0f;
  }

  if (texel.depth < texel.xray_depth) {
    frag.color = float4(srt.opacity);
    return;
  }

  gpu_discard_fragment();
}

PipelineGraphic pipeline(vert_main, frag_main, Resources{});

}  // namespace overlay::xray_fade
