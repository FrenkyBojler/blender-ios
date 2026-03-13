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

/* Test operates on a 4x4 texture patch. */
constexpr uint texture_size_x = 4;
constexpr uint texture_size_y = 4;
constexpr uint texture_size = texture_size_x * texture_size_y;

/* Repeat the first `components` components of a float4 `n` times, into a vector.*/
template<typename T> static Vector<T> repeat_data(VecBase<T, 4> data, size_t n, size_t components)
{
  Vector<T> out(n * components);
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
      "base", texture_size_x, texture_size_y, 1, format, usage, nullptr);
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

/* Create a view texture  of compatible aliasing format. */
static gpu::Texture *create_view_texture(TextureFormat format, gpu::Texture *base)
{
  gpu::Texture *view = GPU_texture_create_view("view", base, format, 0, 1, 0, 1, false, false);
  GPU_texture_mipmap_mode(view, false, false);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);
  return view;
}

/* Read back a texture of type float or half as a contiguous vector of float. */
template<typename T>
static Vector<T> read_texture(gpu::Texture *texture, eGPUDataFormat data_format)
{
  TextureFormat format = GPU_texture_format(texture);

  void *src = GPU_texture_read(texture, data_format, 0);
  Vector<T> dst(texture_size * to_component_len(format));
  std::memcpy(static_cast<void *>(dst.data()), src, sizeof(float) * dst.size());

  MEM_delete_void(src);

  return dst;
}

/* Given a pair of TextureFormat values, create base and view textures and
 * attempt to perform a framebuffer color clear over the view texture. */
template<TextureFormat FormatA, TextureFormat FormatB> static void texture_view_create_test()
{
  /* Note; these tests rely on device to host data conversion for texture readback, which is
   * currently only supported on OpenGL. */
  if (GPU_backend_get_type() != GPU_BACKEND_OPENGL) {
    GTEST_SKIP();
  }

  GPU_render_begin();

  gpu::Texture *base = create_base_texture(FormatA);
  gpu::Texture *view = create_view_texture(FormatB, base);

  /* First check; the view texture should be all zeroes. */
  float4 zero(0.0f, 0.0f, 0.0f, 0.0f);
  if (to_texture_data_format(FormatB) == GPU_DATA_UINT) {
    uint4 uzero(zero);
    auto zero_expected = repeat_data(uzero, texture_size, to_component_len(FormatB));
    auto zero_readback = read_texture<uint>(view, GPU_DATA_UINT);
    EXPECT_TRUE(std::equal(zero_expected.begin(), zero_expected.end(), zero_readback.begin()));
  }
  else if (to_texture_data_format(FormatB) == GPU_DATA_INT) {
    int4 izero(zero);
    auto zero_expected = repeat_data(izero, texture_size, to_component_len(FormatB));
    auto zero_readback = read_texture<int>(view, GPU_DATA_INT);
    EXPECT_TRUE(std::equal(zero_expected.begin(), zero_expected.end(), zero_readback.begin()));
  }
  else if (to_texture_data_format(FormatB) == GPU_DATA_FLOAT) {
    auto zero_expected = repeat_data(zero, texture_size, to_component_len(FormatB));
    auto zero_readback = read_texture<float>(view, GPU_DATA_FLOAT);
    EXPECT_TRUE(std::equal(zero_expected.begin(), zero_expected.end(), zero_readback.begin()));
  }
  else {
    BLI_assert_unreachable();
  }

  /* Create FBO with view as color attachment 0. */
  gpu::FrameBuffer *fbo = nullptr;
  GPU_framebuffer_ensure_config(&fbo, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(view)});
  GPU_framebuffer_bind(fbo);

  /* Clear FBO to specific color with a different value on each channel, all below 2^8. */
  float4 colr = {128.0f, 64.0f, 32.0f, 16.0f};
  GPU_framebuffer_clear(fbo, GPUFrameBufferBits::GPU_COLOR_BIT, double4(colr), 0.0f, 0u);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  /* Second check; the view texture should read back this color. */
  if (to_texture_data_format(FormatB) == GPU_DATA_UINT) {
    uint4 ucolr(colr);
    auto colr_expected = repeat_data(ucolr, texture_size, to_component_len(FormatB));
    auto colr_readback = read_texture<uint>(view, GPU_DATA_UINT);
    EXPECT_TRUE(std::equal(colr_expected.begin(), colr_expected.end(), colr_expected.begin()));
  }
  else if (to_texture_data_format(FormatB) == GPU_DATA_INT) {
    int4 icolr(colr);
    auto colr_expected = repeat_data(icolr, texture_size, to_component_len(FormatB));
    auto colr_readback = read_texture<int>(view, GPU_DATA_INT);
    EXPECT_TRUE(std::equal(colr_expected.begin(), colr_expected.end(), colr_expected.begin()));
  }
  else if (to_texture_data_format(FormatB) == GPU_DATA_FLOAT) {
    auto colr_expected = repeat_data(colr, texture_size, to_component_len(FormatB));
    auto colr_readback = read_texture<float>(view, GPU_DATA_FLOAT);
    EXPECT_TRUE(std::equal(colr_expected.begin(), colr_expected.end(), colr_expected.begin()));
  }
  else {
    BLI_assert_unreachable();
  }

  GPU_framebuffer_free(fbo);
  GPU_texture_free(view);
  GPU_texture_free(base);

  GPU_render_end();
}

static void test_vexture_view_SFLOAT_32_32_32_32()
{
  texture_view_create_test<TextureFormat::SFLOAT_32_32_32_32, TextureFormat::SFLOAT_32_32_32_32>();
  texture_view_create_test<TextureFormat::SFLOAT_32_32_32_32, TextureFormat::UINT_32_32_32_32>();
  texture_view_create_test<TextureFormat::SFLOAT_32_32_32_32, TextureFormat::SINT_32_32_32_32>();
}
GPU_TEST(vexture_view_SFLOAT_32_32_32_32);

static void test_texture_view_SFLOAT_32_32()
{
  texture_view_create_test<TextureFormat::SFLOAT_32_32, TextureFormat::SFLOAT_32_32>();
  texture_view_create_test<TextureFormat::SFLOAT_32_32, TextureFormat::SFLOAT_16_16_16_16>();
  texture_view_create_test<TextureFormat::SFLOAT_32_32, TextureFormat::UINT_32_32>();
  texture_view_create_test<TextureFormat::SFLOAT_32_32, TextureFormat::UINT_16_16_16_16>();
  texture_view_create_test<TextureFormat::SFLOAT_32_32, TextureFormat::SINT_32_32>();
  texture_view_create_test<TextureFormat::SFLOAT_32_32, TextureFormat::SINT_16_16_16_16>();
}
GPU_TEST(texture_view_SFLOAT_32_32);

static void test_texture_view_SFLOAT_32()
{
  texture_view_create_test<TextureFormat::SFLOAT_32, TextureFormat::SFLOAT_32>();
  texture_view_create_test<TextureFormat::SFLOAT_32, TextureFormat::SFLOAT_16_16>();
  texture_view_create_test<TextureFormat::SFLOAT_32, TextureFormat::UINT_32>();
  texture_view_create_test<TextureFormat::SFLOAT_32, TextureFormat::UINT_16_16>();
  texture_view_create_test<TextureFormat::SFLOAT_32, TextureFormat::UINT_8_8_8_8>();
  texture_view_create_test<TextureFormat::SFLOAT_32, TextureFormat::SINT_32>();
  texture_view_create_test<TextureFormat::SFLOAT_32, TextureFormat::SINT_16_16>();
  texture_view_create_test<TextureFormat::SFLOAT_32, TextureFormat::SINT_8_8_8_8>();
}
GPU_TEST(texture_view_SFLOAT_32);

static void test_texture_view_SFLOAT_16()
{
  texture_view_create_test<TextureFormat::SFLOAT_16, TextureFormat::SFLOAT_16>();
  texture_view_create_test<TextureFormat::SFLOAT_16, TextureFormat::UINT_16>();
  texture_view_create_test<TextureFormat::SFLOAT_16, TextureFormat::UINT_8_8>();
  texture_view_create_test<TextureFormat::SFLOAT_16, TextureFormat::SINT_16>();
  texture_view_create_test<TextureFormat::SFLOAT_16, TextureFormat::SINT_8_8>();
}
GPU_TEST(texture_view_SFLOAT_16);

// static void test_texture_view_passthrough()
// {
//   texture_view_create_test<TextureFormat::SFLOAT_32_32_32_32,
//   TextureFormat::SFLOAT_32_32_32_32>();
// }
// GPU_TEST(texture_view_passthrough);

// static void test_texture_view_UINT_32_32_to_SFLOAT_32_32()
// {
//   texture_view_create_test<TextureFormat::UINT_32_32, TextureFormat::SFLOAT_32_32>();
// }
// GPU_TEST(texture_view_UINT_32_32_to_SFLOAT_32_32);

// static void test_texture_view_UINT_16_16_16_16_to_SFLOAT_32_32()
// {
//   texture_view_create_test<TextureFormat::UINT_16_16_16_16, TextureFormat::SFLOAT_32_32>();
// }
// GPU_TEST(texture_view_UINT_16_16_16_16_to_SFLOAT_32_32);

// static void test_texture_view_UINT_32_to_SFLOAT_16_16()
// {
//   texture_view_create_test<TextureFormat::UINT_32, TextureFormat::SFLOAT_16_16>();
// }
// GPU_TEST(texture_view_UINT_32_to_SFLOAT_16_16);

// static void test_texture_view_UINT_32_32_to_SFLOAT_16_16_16_16()
// {
//   texture_view_create_test<TextureFormat::UINT_32_32, TextureFormat::SFLOAT_16_16_16_16>();
// }
// GPU_TEST(texture_view_UINT_32_32_to_SFLOAT_16_16_16_16);

// static void test_texture_view_SFLOAT_32_32_to_SFLOAT_16_16_16_16()
// {
//   texture_view_create_test<TextureFormat::SFLOAT_32_32, TextureFormat::SFLOAT_16_16_16_16>();
// }
// GPU_TEST(texture_view_SFLOAT_32_32_to_SFLOAT_16_16_16_16);

// static void test_texture_view_SFLOAT_32_32_to_UINT_32_32()
// {
//   texture_view_create_test<TextureFormat::SFLOAT_32_32, TextureFormat::UINT_32_32>();
// }
// GPU_TEST(texture_view_SFLOAT_32_32_to_UINT_32_32);

}  // namespace blender::gpu::tests
