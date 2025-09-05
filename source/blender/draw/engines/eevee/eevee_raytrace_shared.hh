/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared code between host and client codebases.
 */

/* __cplusplus is true when compiling with MSL, so ensure we are not inside a shader. */
#if defined(GPU_SHADER) || defined(GLSL_CPP_STUBS)
#  define HOST_CODE 0
#else
#  define HOST_CODE 1
#endif

#pragma once

#include "eevee_transform.hh"

#if HOST_CODE || defined(GLSL_CPP_STUBS)
#  include "eevee_defines.hh"
#endif

#if HOST_CODE
#  include "BLI_math_bits.h"
#  include "DRW_gpu_wrapper.hh"

namespace blender::eevee {
#endif

struct RayTraceData {
  /** ViewProjection matrix used to render the previous frame. */
  float4x4 history_persmat;
  /** ViewProjection matrix used to render the radiance texture. */
  float4x4 radiance_persmat;
  /** Input resolution. */
  int2 full_resolution;
  /** Inverse of input resolution to get screen UVs. */
  float2 full_resolution_inv;
  /** Scale and bias to go from ray-trace resolution to input resolution. */
  int2 resolution_bias;
  int resolution_scale;
  /** View space thickness the objects. */
  float thickness;
  /** Scale and bias to go from horizon-trace resolution to input resolution. */
  int2 horizon_resolution_bias;
  int horizon_resolution_scale;
  /** Determine how fast the sample steps are getting bigger. */
  float quality;
  /** Maximum roughness for which we will trace a ray. */
  float roughness_mask_scale;
  float roughness_mask_bias;
  /** If set to true will bypass spatial denoising. */
  bool32_t skip_denoise;
  /** If set to false will bypass tracing for refractive closures. */
  bool32_t trace_refraction;
  /** Closure being ray-traced. */
  int closure_index;
  int _pad0;
  int _pad1;
};
BLI_STATIC_ASSERT_ALIGN(RayTraceData, 16)

struct AOData {
  float2 pixel_size;
  float distance;
  float lod_factor;

  float thickness_near;
  float thickness_far;
  float angle_bias;
  float gi_distance;

  float lod_factor_ao;
  float _pad0;
  float _pad1;
  float _pad2;
};
BLI_STATIC_ASSERT_ALIGN(AOData, 16)

#if HOST_CODE
using RayTraceTileBuf = draw::StorageArrayBuffer<uint, 1024, true>;
}  // namespace blender::eevee
#endif
