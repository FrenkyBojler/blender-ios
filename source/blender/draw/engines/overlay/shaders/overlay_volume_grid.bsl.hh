/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "draw_view_infos.hh"

SHADER_LIBRARY_CREATE_INFO(draw_modelmat)

#include "draw_model_lib.glsl"
#include "gpu_shader_utildefines_lib.glsl"

namespace workbench::overlay_volume_grid {

struct Resources {
  // [[image(1, read, SFLOAT_32_32_32_32)]]
  // const image1D positions;
  
  [[sampler(1) /*, frequency(batch)*/]] samplerBuffer positions;
  
  [[legacy_info]]
  ShaderCreateInfo draw_modelmat;
  
  [[legacy_info]]
  ShaderCreateInfo draw_view;

  [[push_constant]] const float4x4 grid_transform;
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
  const int voxel_index = vert_id / 24;
  const int vert_index = vert_id % 24;

  const float3 position = texelFetch(resources.positions, voxel_index).rgb;

  float3 vert_pos;
  switch (vert_index) {
    case (0 * 2 + 0): { vert_pos = float3(1.0, 1.0, 1.0); break; }
    case (0 * 2 + 1): { vert_pos = float3(-1.0, 1.0, 1.0); break; }
    
    case (1 * 2 + 0): { vert_pos = float3(1.0, -1.0, 1.0); break; }
    case (1 * 2 + 1): { vert_pos = float3(-1.0, -1.0, 1.0); break; }
    
    case (2 * 2 + 0): { vert_pos = float3(1.0, 1.0, -1.0); break; }
    case (2 * 2 + 1): { vert_pos = float3(-1.0, 1.0, -1.0); break; }
    
    case (3 * 2 + 0): { vert_pos = float3(1.0, -1.0, -1.0); break; }
    case (3 * 2 + 1): { vert_pos = float3(-1.0, -1.0, -1.0); break; }


    case (4 * 2 + 0): { vert_pos = float3(1.0, 1.0, 1.0); break; }
    case (4 * 2 + 1): { vert_pos = float3(1.0, -1.0, 1.0); break; }
    
    case (5 * 2 + 0): { vert_pos = float3(-1.0, 1.0, 1.0); break; }
    case (5 * 2 + 1): { vert_pos = float3(-1.0, -1.0, 1.0); break; }
    
    case (6 * 2 + 0): { vert_pos = float3(1.0, 1.0, -1.0); break; }
    case (6 * 2 + 1): { vert_pos = float3(1.0, -1.0, -1.0); break; }
    
    case (7 * 2 + 0): { vert_pos = float3(-1.0, 1.0, -1.0); break; }
    case (7 * 2 + 1): { vert_pos = float3(-1.0, -1.0, -1.0); break; }


    case (8 * 2 + 0): { vert_pos = float3(1.0, 1.0, 1.0); break; }
    case (8 * 2 + 1): { vert_pos = float3(1.0, 1.0, -1.0); break; }
    
    case (9 * 2 + 0): { vert_pos = float3(-1.0, 1.0, 1.0); break; }
    case (9 * 2 + 1): { vert_pos = float3(-1.0, 1.0, -1.0); break; }
    
    case (10 * 2 + 0): { vert_pos = float3(1.0, -1.0, 1.0); break; }
    case (10 * 2 + 1): { vert_pos = float3(1.0, -1.0, -1.0); break; }
    
    case (11 * 2 + 0): { vert_pos = float3(-1.0, -1.0, 1.0); break; }
    case (11 * 2 + 1): { vert_pos = float3(-1.0, -1.0, -1.0); break; }
  }

  float3 world_pos = drw_point_object_to_world((resources.grid_transform * float4(position + vert_pos / 4.0f, 1.0f)).xyz);
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