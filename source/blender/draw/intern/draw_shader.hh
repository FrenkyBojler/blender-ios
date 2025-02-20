/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 */

#pragma once

#include "draw_curves_private.hh"
#include "draw_hair_private.hh"

struct GPUShader;

/* draw_shader.cc */

GPUShader *DRW_shader_hair_refine_get(ParticleRefineShader refinement);

GPUShader *DRW_shader_curves_refine_get(blender::draw::CurvesEvalShader type);

GPUShader *DRW_shader_debug_draw_display_get();
GPUShader *DRW_shader_draw_visibility_compute_get();
GPUShader *DRW_shader_draw_view_finalize_get();
GPUShader *DRW_shader_draw_resource_finalize_get();
GPUShader *DRW_shader_draw_command_generate_get();

/* Subdivision */
enum class SubdivShaderType {
  BUFFER_LINES = 0,
  BUFFER_LINES_LOOSE = 1,
  BUFFER_EDGE_FAC = 2,
  BUFFER_LNOR = 3,
  BUFFER_TRIS = 4,
  BUFFER_TRIS_MULTIPLE_MATERIALS = 5,
  BUFFER_NORMALS_ACCUMULATE = 6,
  BUFFER_NORMALS_FINALIZE = 7,
  BUFFER_CUSTOM_NORMALS_FINALIZE = 8,
  PATCH_EVALUATION = 9,
  PATCH_EVALUATION_FVAR = 10,
  PATCH_EVALUATION_FACE_DOTS = 11,
  PATCH_EVALUATION_FACE_DOTS_WITH_NORMALS = 12,
  PATCH_EVALUATION_ORCO = 13,
  COMP_CUSTOM_DATA_INTERP_1D = 14,
  COMP_CUSTOM_DATA_INTERP_2D = 15,
  COMP_CUSTOM_DATA_INTERP_3D = 16,
  COMP_CUSTOM_DATA_INTERP_4D = 17,
  BUFFER_SCULPT_DATA = 18,
  BUFFER_UV_STRETCH_ANGLE = 19,
  BUFFER_UV_STRETCH_AREA = 20,
};
constexpr int SUBDIVISION_MAX_SHADERS = 21;

GPUShader *DRW_shader_subdiv_get(SubdivShaderType shader_type);
GPUShader *DRW_shader_subdiv_custom_data_get(GPUVertCompType comp_type, int dimensions);

void DRW_shaders_free();

#include "BLI_utility_mixins.hh"
#include "GPU_shader.hh"
#include <mutex>

namespace blender::draw {

class StaticShader : NonCopyable {
 private:
  std::string info_name_;
  GPUShader *shader_ = nullptr;
  bool failed_ = false;
  std::mutex mutex_;

  void move(StaticShader &&other)
  {
    std::scoped_lock lock1(mutex_);
    std::scoped_lock lock2(other.mutex_);
    BLI_assert(shader_ == nullptr && info_name_.empty());
    std::swap(info_name_, other.info_name_);
    std::swap(shader_, other.shader_);
    std::swap(failed_, other.failed_);
  }

 public:
  StaticShader(std::string info_name) : info_name_(info_name) {}

  StaticShader() = default;
  StaticShader(StaticShader &&other)
  {
    move(std::move(other));
  }
  StaticShader &operator=(StaticShader &&other)
  {
    move(std::move(other));
    return *this;
  };

  ~StaticShader()
  {
    GPU_SHADER_FREE_SAFE(shader_);
  }

  GPUShader *get()
  {
    if (!shader_ && !failed_) {
      std::scoped_lock lock(mutex_);
      /* Check again in case it was created between first check and lock. */
      if (!shader_ && !failed_) {
        BLI_assert(!info_name_.empty());
        shader_ = GPU_shader_create_from_info_name(info_name_.c_str());
        failed_ = shader_ != nullptr;
      }
    }
    return shader_;
  }

  /* For batch compiled shaders. */
  /* TODO: Find a better way to handle this. */
  void set(GPUShader *shader)
  {
    std::scoped_lock lock(mutex_);
    BLI_assert(shader_ == nullptr);
    shader_ = shader;
  }
};

}  // namespace blender::draw
