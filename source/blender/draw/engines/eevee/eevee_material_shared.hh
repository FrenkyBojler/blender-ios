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

#if HOST_CODE
namespace blender::eevee {
#endif

enum eMaterialPipeline {
  MAT_PIPE_DEFERRED = 0,
  MAT_PIPE_FORWARD,
  /* These all map to the depth shader. */
  MAT_PIPE_PREPASS_DEFERRED,
  MAT_PIPE_PREPASS_DEFERRED_VELOCITY,
  MAT_PIPE_PREPASS_OVERLAP,
  MAT_PIPE_PREPASS_FORWARD,
  MAT_PIPE_PREPASS_FORWARD_VELOCITY,
  MAT_PIPE_PREPASS_PLANAR,

  MAT_PIPE_VOLUME_MATERIAL,
  MAT_PIPE_VOLUME_OCCUPANCY,
  MAT_PIPE_SHADOW,
  MAT_PIPE_CAPTURE,
};

enum eMaterialGeometry {
  /* These maps directly to object types. */
  MAT_GEOM_MESH = 0,
  MAT_GEOM_POINTCLOUD,
  MAT_GEOM_CURVES,
  MAT_GEOM_VOLUME,

  /* These maps to special shader. */
  MAT_GEOM_WORLD,
};

#if HOST_CODE
}  // namespace blender::eevee
#endif
