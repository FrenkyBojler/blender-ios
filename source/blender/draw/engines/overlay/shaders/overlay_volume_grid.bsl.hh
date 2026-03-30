/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_view_infos.hh"

SHADER_LIBRARY_CREATE_INFO(draw_modelmat)

#include "draw_model_lib.glsl"

namespace workbench::overlay_volume_grid {

struct Resources {
  [[image(1, read, SFLOAT_32_32_32_32)]]
  const image1D positions;
  [[legacy_info]]
  ShaderCreateInfo draw_modelmat;
  [[legacy_info]]
  ShaderCreateInfo draw_view;
};

struct FragOut {
  [[frag_color(0)]]
  float4 color;
};

[[vertex]]
void vert_main([[vertex_id]]
               const int &vert_id,
               [[resource_table]]
               const Resources &resources,
               [[position]]
               float4 &out_pos)
{
  switch (vert_id % 3) {
    case 0: { out_pos = float4(1, 1, 0, 0); break; }
    case 1: { out_pos = float4(1, 0, 0, 0); break; }
    default: { out_pos = float4(0, 0, 0, 0); break; }
  }

  float3 world_pos = drw_point_object_to_world(out_pos.xyz);
  out_pos = drw_point_world_to_homogenous(world_pos);
}

[[fragment]]
void frag_main([[frag_coord]]
               const float4 &frag_coord,
               [[out]]
               FragOut &frag)
{
  frag.color = float4(1.0f, 1.0f, 1.0f, 1.0f);
}

PipelineGraphic pipline(vert_main, frag_main);

}  // namespace workbench::overlay_volume_grid