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

#define VELOCITY_INVALID 512.0

enum eVelocityStep : uint32_t {
  STEP_PREVIOUS = 0,
  STEP_NEXT = 1,
  STEP_CURRENT = 2,
};

struct VelocityObjectIndex {
  /** Offset inside #VelocityObjectBuf for each time-step. Indexed using eVelocityStep. */
  packed_int3 ofs;
  /** Temporary index to copy this to the #VelocityIndexBuf. */
  uint resource_id;

#ifdef HOST_CODE
  VelocityObjectIndex() : ofs(-1, -1, -1), resource_id(-1){};
#endif
};
BLI_STATIC_ASSERT_ALIGN(VelocityObjectIndex, 16)

struct VelocityGeometryIndex {
  /** Offset inside #VelocityGeometryBuf for each time-step. Indexed using eVelocityStep. */
  packed_int3 ofs;
  /** If true, compute deformation motion blur. */
  bool32_t do_deform;
  /**
   * Length of data inside #VelocityGeometryBuf for each time-step.
   * Indexed using eVelocityStep.
   */
  packed_int3 len;

  int _pad0;

#ifdef HOST_CODE
  VelocityGeometryIndex() : ofs(-1, -1, -1), do_deform(false), len(-1, -1, -1), _pad0(1){};
#endif
};
BLI_STATIC_ASSERT_ALIGN(VelocityGeometryIndex, 16)

struct VelocityIndex {
  VelocityObjectIndex obj;
  VelocityGeometryIndex geo;
};
BLI_STATIC_ASSERT_ALIGN(VelocityGeometryIndex, 16)

#if HOST_CODE
using VelocityGeometryBuf = draw::StorageArrayBuffer<float4, 16, true>;
using VelocityIndexBuf = draw::StorageArrayBuffer<VelocityIndex, 16>;
using VelocityObjectBuf = draw::StorageArrayBuffer<float4x4, 16>;
}  // namespace blender::eevee
#endif
