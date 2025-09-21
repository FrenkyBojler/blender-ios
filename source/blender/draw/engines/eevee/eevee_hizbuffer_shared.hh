/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared code between host and client codebases.
 */

#pragma once

#include "GPU_shader_shared_utils.hh"

#ifndef GPU_SHADER
namespace blender::eevee {
#endif

struct HiZData {
  /** Scale factor to remove HiZBuffer padding. */
  float2 uv_scale;

  int2 render_extent;
};
BLI_STATIC_ASSERT_ALIGN(HiZData, 16)

#ifndef GPU_SHADER
}  // namespace blender::eevee
#endif
