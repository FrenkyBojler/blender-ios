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

#if HOST_CODE || defined(GLSL_CPP_STUBS)
#  include "GPU_shader_shared_utils.hh"
#endif

#if HOST_CODE
#  include "DRW_gpu_wrapper.hh"

namespace blender::eevee {
#endif

#define MOTION_BLUR_TILE_SIZE 32
#define MOTION_BLUR_MAX_TILE 512 /* 16384 / MOTION_BLUR_TILE_SIZE */
struct MotionBlurData {
  /** As the name suggests. Used to avoid a division in the sampling. */
  float2 target_size_inv;
  /** Viewport motion scaling factor. Make blur relative to frame time not render time. */
  float2 motion_scale;
  /** Depth scaling factor. Avoid blurring background behind moving objects. */
  float depth_scale;

  float _pad0, _pad1, _pad2;
};
BLI_STATIC_ASSERT_ALIGN(MotionBlurData, 16)

/* For some reasons some GLSL compilers do not like this struct.
 * So we declare it as a uint array instead and do indexing ourselves. */
#ifdef __cplusplus
struct MotionBlurTileIndirection {
  /**
   * Stores indirection to the tile with the highest velocity covering each tile.
   * This is stored using velocity in the MSB to be able to use atomicMax operations.
   */
  uint prev[MOTION_BLUR_MAX_TILE][MOTION_BLUR_MAX_TILE];
  uint next[MOTION_BLUR_MAX_TILE][MOTION_BLUR_MAX_TILE];
};
BLI_STATIC_ASSERT_ALIGN(MotionBlurTileIndirection, 16)
#endif

#if HOST_CODE
using MotionBlurDataBuf = draw::UniformBuffer<MotionBlurData>;
using MotionBlurTileIndirectionBuf = draw::StorageBuffer<MotionBlurTileIndirection, true>;
}  // namespace blender::eevee
#endif
