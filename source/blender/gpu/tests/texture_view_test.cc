/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "gpu_testing.hh"

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_math_half.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "GPU_context.hh"
#include "GPU_framebuffer.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "gpu_texture_private.hh"

#include <bitset>

namespace blender::gpu::tests {

constexpr uint texture_size_x = 4;
constexpr uint texture_size_total = texture_size_x * texture_size_x;

static Vector<float> repeat_components_from(float4 data, size_t components, size_t repeats)
{
  Vector<float> out(components * repeats);
  for (uint i = 0; i < out.size(); ++i) {
    out[i] = data[i % components];
  }
  return out;
}

/* Create a base texture of the specified format and clear it to black. */
static gpu::Texture *create_base_texture(TextureFormat format)
{
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL | GPU_TEXTURE_USAGE_HOST_READ |
                                     GPU_TEXTURE_USAGE_FORMAT_VIEW;

  gpu::Texture *base = GPU_texture_create_2d(
      "base", texture_size_x, texture_size_x, 1, format, usage, nullptr);
  GPU_texture_mipmap_mode(base, false, false);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);

  /* Bind the texture as attachment to a temporary framebuffer, and clear to black. */
  gpu::FrameBuffer *fbo = nullptr;
  GPU_framebuffer_ensure_config(&fbo, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(base)});
  GPU_framebuffer_bind(fbo);
  GPU_framebuffer_clear(fbo, GPUFrameBufferBits::GPU_COLOR_BIT, {0.0, 0.0, 0.0, 0.0}, 0.0f, 0u);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  GPU_framebuffer_free(fbo);

  return base;
}

/* Create a view texture over base, of compatible aliasing format. */
static gpu::Texture *create_view_texture(TextureFormat format, gpu::Texture *base)
{
  gpu::Texture *view = GPU_texture_create_view("view", base, format, 0, 1, 0, 1, false, false);
  GPU_texture_mipmap_mode(view, false, false);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);
  return view;
}

/* Read back a n*n, n-channel texture of type float or half, return cast to float4. */
static Vector<float> read_texture(gpu::Texture *texture)
{
  TextureFormat format = GPU_texture_format(texture);

  void *src = GPU_texture_read(texture, GPU_DATA_FLOAT, 0);
  Vector<float> dst(texture_size_total * to_component_len(format));
  std::memcpy(static_cast<void *>(dst.data()), src, sizeof(float) * dst.size());

  MEM_delete_void(src);

  return dst;
}

/* Given a pair of TextureFormat values, create base and view textures and
 * attempt to perform a framebuffer color clear over the view texture. */
template<TextureFormat FormatA, TextureFormat FormatB> static void texture_view_create_test()
{
  GPU_render_begin();

  if (GPU_backend_get_type() != GPU_BACKEND_OPENGL) {
    GTEST_SKIP();
  }

  gpu::Texture *base = create_base_texture(FormatA);
  gpu::Texture *view = create_view_texture(FormatB, base);

  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);

  /* First check; the view texture should be all zeroes. */
  float4 zero_color(0.0f, 0.0f, 0.0f, 0.0f);

  auto zero_color_compare = repeat_components_from(
      zero_color, to_component_len(FormatB), texture_size_total);
  auto zero_color_readback = read_texture(view);
  EXPECT_TRUE(std::equal(
      zero_color_compare.begin(), zero_color_compare.end(), zero_color_readback.begin()));

  /* Create FBO with view as color attachment 0. */
  gpu::FrameBuffer *fbo = nullptr;
  GPU_framebuffer_ensure_config(&fbo, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(view)});
  GPU_framebuffer_bind(fbo);

  /* Clear FBO to specific color. */
  float4 test_color = {2.0, 0.25, 1.25, 0.25};
  for (uint i = to_component_len(FormatB); i < 4; ++i) {
    test_color[i] = 0.0;
  }
  GPU_framebuffer_clear(fbo, GPUFrameBufferBits::GPU_COLOR_BIT, double4(test_color), 0.0f, 0u);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  /* Second check; the view texture should read back this color. */
  auto test_color_compare = repeat_components_from(
      test_color, to_component_len(FormatB), texture_size_total);
  auto test_color_readback = read_texture(view);
  EXPECT_TRUE(std::equal(
      test_color_compare.begin(), test_color_compare.end(), test_color_compare.begin()));

  GPU_framebuffer_free(fbo);
  GPU_texture_free(view);
  GPU_texture_free(base);

  GPU_render_end();
}

static void test_texture_view_passthrough()
{
  texture_view_create_test<TextureFormat::SFLOAT_32_32_32_32, TextureFormat::SFLOAT_32_32_32_32>();
}
GPU_TEST(texture_view_passthrough);

static void test_texture_view_UINT_32_32_to_SFLOAT_32_32()
{
  texture_view_create_test<TextureFormat::UINT_32_32, TextureFormat::SFLOAT_32_32>();
}
GPU_TEST(texture_view_UINT_32_32_to_SFLOAT_32_32);

static void test_texture_view_UINT_16_16_16_16_to_SFLOAT_32_32()
{
  texture_view_create_test<TextureFormat::UINT_16_16_16_16, TextureFormat::SFLOAT_32_32>();
}
GPU_TEST(texture_view_UINT_16_16_16_16_to_SFLOAT_32_32);

static void test_texture_view_UINT_32_to_SFLOAT_16_16()
{
  texture_view_create_test<TextureFormat::UINT_32, TextureFormat::SFLOAT_16_16>();
}
GPU_TEST(texture_view_UINT_32_to_SFLOAT_16_16);

static void test_texture_view_UINT_32_32_to_SFLOAT_16_16_16_16()
{
  texture_view_create_test<TextureFormat::UINT_32_32, TextureFormat::SFLOAT_16_16_16_16>();
}
GPU_TEST(texture_view_UINT_32_32_to_SFLOAT_16_16_16_16);

}  // namespace blender::gpu::tests
