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

#include "eevee_camera_shared.hh"
#include "eevee_film_shared.hh"
#include "eevee_hizbuffer_shared.hh"
#include "eevee_raytrace_shared.hh"
#include "eevee_renderbuffers_shared.hh"
#include "eevee_shadow_shared.hh"
#include "eevee_subsurface_shared.hh"
#include "eevee_volume_shared.hh"

#if HOST_CODE || defined(GLSL_CPP_STUBS)
#  include "GPU_shader_shared_utils.hh"
#endif

#if HOST_CODE
#  include "DRW_gpu_wrapper.hh"

namespace blender::eevee {
#endif

/* Light Clamping. */
struct ClampData {
  float sun_threshold;
  float surface_direct;
  float surface_indirect;
  float volume_direct;
  float volume_indirect;
  float _pad0;
  float _pad1;
  float _pad2;
};
BLI_STATIC_ASSERT_ALIGN(ClampData, 16)

struct PipelineInfoData {
  float alpha_hash_scale;
  bool32_t is_sphere_probe;
  float _pad1;
  float _pad2;
};
BLI_STATIC_ASSERT_ALIGN(PipelineInfoData, 16)

/* Combines data from several modules to avoid wasting binding slots. */
struct UniformData {
  AOData ao;
  CameraData camera;
  ClampData clamp;
  FilmData film;
  HiZData hiz;
  RayTraceData raytrace;
  RenderBuffersInfoData render_pass;
  ShadowSceneData shadow;
  SubsurfaceData subsurface;
  VolumesInfoData volumes;
  PipelineInfoData pipeline;
};
BLI_STATIC_ASSERT_ALIGN(UniformData, 16)

/**
 * World space clip plane equation. Used to render planar light-probes.
 * Moved here to avoid dependencies to light-probe just for this. */
struct ClipPlaneData {
  float4 plane;
};
BLI_STATIC_ASSERT_ALIGN(ClipPlaneData, 16)

#if HOST_CODE
using ClipPlaneBuf = draw::UniformBuffer<ClipPlaneData>;
using UniformDataBuf = draw::UniformBuffer<UniformData>;
}  // namespace blender::eevee
#endif
