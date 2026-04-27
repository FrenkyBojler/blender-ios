/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Shared code between host and client code-bases.
 */

#pragma once

#ifndef GPU_SHADER
namespace blender::eevee {
#endif

/**
 * Defines the specific rendering pass or shading strategy for a material.
 * This determines which shader variants are generated.
 */
enum eMaterialPipeline {
  /* G-Buffer pass, lighting is calculated in a separate pass. */
  MAT_PIPE_DEFERRED = 0,
  /* Main shading pass where lighting is calculated per-pixel during geometry submission. */
  MAT_PIPE_FORWARD,

  /**
   * Pre-pass shaders: These are used to populate the Depth buffer and Motion Vectors before
   * the main shading pass.
   */

  /* Standard depth-only pass for the deferred pipeline. */
  MAT_PIPE_PREPASS_DEFERRED,
  MAT_PIPE_PREPASS_DEFERRED_VELOCITY,
  MAT_PIPE_PREPASS_DEFERRED_RAYCAST,
  MAT_PIPE_PREPASS_DEFERRED_VELOCITY_RAYCAST,
  /* Standard depth-only pass for the forward pipeline (opaque only). */
  MAT_PIPE_PREPASS_FORWARD,
  MAT_PIPE_PREPASS_FORWARD_VELOCITY,
  MAT_PIPE_PREPASS_FORWARD_RAYCAST,
  MAT_PIPE_PREPASS_FORWARD_VELOCITY_RAYCAST,
  /* Per object prepass to handle the transparency overlap option. */
  MAT_PIPE_PREPASS_OVERLAP,
  /* Depth pre-pass specifically for planar reflection probes. */
  MAT_PIPE_PREPASS_PLANAR,

  /* Pipeline for baking meshes volume occupancy to the froxel grid. */
  MAT_PIPE_VOLUME_OCCUPANCY,
  /* Pipeline for baking volume material properties to the froxel grid. */
  MAT_PIPE_VOLUME_MATERIAL,

  /* Pipeline for shadow map rendering. */
  MAT_PIPE_SHADOW,

  /* Pipeline for surfel capture. */
  MAT_PIPE_CAPTURE,
};

constexpr eMaterialPipeline pipeline_prepass_get(bool is_forward,
                                                 bool with_velocity,
                                                 bool with_raycast)
{
  return is_forward ?
             (with_velocity ?
                  (with_raycast ? MAT_PIPE_PREPASS_FORWARD_VELOCITY_RAYCAST :
                                  MAT_PIPE_PREPASS_FORWARD_VELOCITY) :
                  (with_raycast ? MAT_PIPE_PREPASS_FORWARD_RAYCAST : MAT_PIPE_PREPASS_FORWARD)) :
             (with_velocity ?
                  (with_raycast ? MAT_PIPE_PREPASS_DEFERRED_VELOCITY_RAYCAST :
                                  MAT_PIPE_PREPASS_DEFERRED_VELOCITY) :
                  (with_raycast ? MAT_PIPE_PREPASS_DEFERRED_RAYCAST : MAT_PIPE_PREPASS_DEFERRED));
};

constexpr bool pipeline_is_forward(eMaterialPipeline pipeline)
{
  switch (pipeline) {
    case MAT_PIPE_FORWARD:
    case MAT_PIPE_PREPASS_FORWARD:
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY:
    case MAT_PIPE_PREPASS_FORWARD_RAYCAST:
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY_RAYCAST:
    case MAT_PIPE_PREPASS_OVERLAP:
    case MAT_PIPE_PREPASS_PLANAR:
      return true;
    default:
      return false;
  }
}

constexpr bool pipeline_is_prepass(eMaterialPipeline pipeline)
{
  switch (pipeline) {
    case MAT_PIPE_PREPASS_DEFERRED:
    case MAT_PIPE_PREPASS_DEFERRED_VELOCITY:
    case MAT_PIPE_PREPASS_DEFERRED_RAYCAST:
    case MAT_PIPE_PREPASS_DEFERRED_VELOCITY_RAYCAST:
    case MAT_PIPE_PREPASS_FORWARD:
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY:
    case MAT_PIPE_PREPASS_FORWARD_RAYCAST:
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY_RAYCAST:
    case MAT_PIPE_PREPASS_OVERLAP:
    case MAT_PIPE_PREPASS_PLANAR:
      return true;
    default:
      return false;
  }
}

constexpr bool pipeline_has_velocity(eMaterialPipeline pipeline)
{
  switch (pipeline) {
    case MAT_PIPE_PREPASS_DEFERRED_VELOCITY:
    case MAT_PIPE_PREPASS_DEFERRED_VELOCITY_RAYCAST:
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY:
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY_RAYCAST:
      return true;
    default:
      return false;
  }
}

constexpr bool pipeline_is_raycast_target(eMaterialPipeline pipeline)
{
  switch (pipeline) {
    case MAT_PIPE_PREPASS_DEFERRED_RAYCAST:
    case MAT_PIPE_PREPASS_DEFERRED_VELOCITY_RAYCAST:
    case MAT_PIPE_PREPASS_FORWARD_RAYCAST:
    case MAT_PIPE_PREPASS_FORWARD_VELOCITY_RAYCAST:
      return true;
    default:
      return false;
  }
}

/**
 * Defines the geometric primitive type the shader is intended to run on.
 * This affects attribute fetching and attribute interpolation.
 */
enum eMaterialGeometry {
  /* These maps directly to object types. */
  MAT_GEOM_MESH = 0,
  MAT_GEOM_POINTCLOUD,
  MAT_GEOM_CURVES,
  MAT_GEOM_VOLUME,

  /* Special case: The world background / HDRI environment shader. */
  MAT_GEOM_WORLD,
};

#ifndef GPU_SHADER
}  // namespace blender::eevee
#endif
