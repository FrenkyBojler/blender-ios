/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "gpu_shader_create_info.hh"
#include "gpu_shader_create_info_private.hh"
#include "gpu_testing.hh"

namespace blender::gpu::tests {

using namespace blender::gpu::shader;

/**
 * Test if all static shaders can be compiled.
 */
static void test_static_shaders()
{
  if (GPU_type_matches_ex(
          GPU_DEVICE_NVIDIA, GPU_OS_ANY, GPU_DRIVER_OFFICIAL, GPU_BACKEND_OPENGL) &&
      G.debug & G_DEBUG_GPU_FORCE_WORKAROUNDS)
  {
    GTEST_SKIP() << "NVIDIA fails to compile workaround due to reserved names. Gladly it doesn't "
                    "need the workaround.";
  }

  EXPECT_TRUE(gpu_shader_create_info_compile_all(nullptr));
}
GPU_TEST(static_shaders)

static void test_shader_create_info_pipeline()
{
  if (GPU_type_matches_ex(
          GPU_DEVICE_NVIDIA, GPU_OS_ANY, GPU_DRIVER_OFFICIAL, GPU_BACKEND_OPENGL) &&
      G.debug & G_DEBUG_GPU_FORCE_WORKAROUNDS)
  {
    GTEST_SKIP() << "NVIDIA fails to compile workaround due to reserved names. Gladly it doesn't "
                    "need the workaround.";
  }

  ShaderCreateInfo create_info("gpu_framebuffer_layer_viewport_test");
  create_info.vertex_source("gpu_framebuffer_layer_viewport_test.glsl");
  create_info.fragment_source("gpu_framebuffer_layer_viewport_test.glsl");
  create_info.builtins(BuiltinBits::VIEWPORT_INDEX | BuiltinBits::LAYER | BuiltinBits::VERTEX_ID);
  create_info.fragment_out(0, Type::int2_t, "out_value");

  create_info.pipeline_state()
      .write_mask(GPU_WRITE_COLOR)
      .primitive(GPU_PRIM_TRIS)
      .blend_mode(GPU_BLEND_NONE)
      .viewports(16)
      .color_format(TextureFormat::SINT_32_32);

  Shader *shader = GPU_shader_create_from_info(
      reinterpret_cast<GPUShaderCreateInfo *>(&create_info));

  EXPECT_TRUE(shader != nullptr);

  GPU_SHADER_FREE_SAFE(shader);
}
GPU_TEST(shader_create_info_pipeline)

}  // namespace blender::gpu::tests
