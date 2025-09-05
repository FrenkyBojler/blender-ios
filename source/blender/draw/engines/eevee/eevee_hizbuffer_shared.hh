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
#  include "DRW_gpu_wrapper.hh"

namespace blender::eevee {
#endif

struct HiZData {
  /** Scale factor to remove HiZBuffer padding. */
  float2 uv_scale;

  float2 _pad0;
};
BLI_STATIC_ASSERT_ALIGN(HiZData, 16)

#if HOST_CODE
}  // namespace blender::eevee
#endif
