/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_glsl_cpp_stubs.hh"

#ifdef GPU_FRAGMENT_SHADER
float texture_streaming_get_lod(uint2 resolution, float2 uv_dx, float2 uv_dy)
{
  return log2(max(length(uv_dx * resolution), length(uv_dy * resolution)));
}

void texture_streaming_write_feedback(int ima_info, float2 uv_dx, float2 uv_dy)
{
  float lod_uvset0 = texture_streaming_get_lod(uint2(65536u), uv_dx, uv_dy);
  uint resolution = 65536u >> uint(max(0, lod_uvset0));
  uint mask = resolution;
  atomicOr(out_texture_lod[ima_info], mask);
}
#else
void texture_streaming_write_feedback(int ima_info)
{
  uint mask = 65536u;
  atomicOr(out_texture_lod[ima_info], mask);
}
#endif
