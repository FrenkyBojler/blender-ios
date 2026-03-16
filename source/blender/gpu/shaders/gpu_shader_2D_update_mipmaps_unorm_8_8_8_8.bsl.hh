/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once
#pragma create_info

#include "gpu_shader_2D_update_mipmaps_lib.bsl.hh"
#include "gpu_shader_create_info.hh"

namespace builtin::mipmaps {

struct UpdateMipmapsUNORM_8_8_8_8 {
  [[push_constant]] const int num_levels;
  [[image(0, read, UNORM_8_8_8_8)]] image2D mip0;
  [[image(1, write, UNORM_8_8_8_8)]] image2D mip1;
  [[image(2, write, UNORM_8_8_8_8)]] image2D mip2;
  [[image(3, write, UNORM_8_8_8_8)]] image2D mip3;
  [[image(4, write, UNORM_8_8_8_8)]] image2D mip4;
  [[image(5, write, UNORM_8_8_8_8)]] image2D mip5;
  [[image(6, write, UNORM_8_8_8_8)]] image2D mip6;
  [[image(7, write, UNORM_8_8_8_8)]] image2D mip7;
};

template<typename SRT> void update_mipmaps_local(const uint3 global_id, SRT &srt)
{
  uint y = global_id.y;
}

template void update_mipmaps_local<UpdateMipmapsUNORM_8_8_8_8>(const uint3 global_id,
                                                               UpdateMipmapsUNORM_8_8_8_8 &srt);

[[local_size(16)]] [[compute]]
void update_mipmaps_UNORM_8_8_8_8([[global_invocation_id]] const uint3 global_id,
                                  [[resource_table]] UpdateMipmapsUNORM_8_8_8_8 &srt)
{
  update_mipmaps_local<UpdateMipmapsUNORM_8_8_8_8>(global_id, srt);
}

}  // namespace builtin::mipmaps

PipelineCompute gpu_shader_2D_update_mipmaps_unorm_8_8_8_8(
    builtin::mipmaps::update_mipmaps_UNORM_8_8_8_8,
    builtin::mipmaps::UpdateMipmapsUNORM_8_8_8_8{});
