/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "gpu_testing.hh"

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"

#include "GPU_context.hh"
#include "GPU_framebuffer.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "gpu_texture_private.hh"

namespace blender::gpu::tests {

/* Arrays overlapping (A) the internal formats table of glTextureView, and (B)
 * the supported types in gpu::TextureTargetFormat::. This excludes compressed
 * formats, depth textures, as these remain unsupported. */
constexpr auto formats_class_128 = {
    gpu::TextureFormat::SFLOAT_32_32_32_32,
    gpu::TextureFormat::UINT_32_32_32_32,
    gpu::TextureFormat::SINT_32_32_32_32,
};
constexpr auto formats_class_64 = {
    gpu::TextureFormat::SFLOAT_16_16_16_16,
    gpu::TextureFormat::SFLOAT_32_32,
    gpu::TextureFormat::UINT_16_16_16_16,
    gpu::TextureFormat::UINT_32_32,
    gpu::TextureFormat::SINT_16_16_16_16,
    gpu::TextureFormat::SINT_32_32,
    gpu::TextureFormat::UNORM_16_16_16_16,
    /* ::SNORM_16_16_16_16, */ /* Not of TextureTargetFormat. */
};
constexpr auto formats_class_32 = {
    gpu::TextureFormat::SFLOAT_16_16,
    gpu::TextureFormat::UFLOAT_11_11_10,
    gpu::TextureFormat::SFLOAT_32,
    // gpu::TextureFormat::UINT_10_10_10_2, /* Unused. Breaks on Intel. */
    gpu::TextureFormat::UINT_8_8_8_8,
    gpu::TextureFormat::UINT_16_16,
    gpu::TextureFormat::UINT_32,
    gpu::TextureFormat::SINT_8_8_8_8,
    gpu::TextureFormat::SINT_16_16,
    gpu::TextureFormat::SINT_32,
    gpu::TextureFormat::UNORM_10_10_10_2,
    gpu::TextureFormat::UNORM_8_8_8_8,
    gpu::TextureFormat::UNORM_16_16,
    /* ::SNORM_8_8_8_8, */ /* Not of TextureTargetFormat. */
    /* ::SNORM_16_16, */   /* Not of TextureTargetFormat. */
    gpu::TextureFormat::SRGBA_8_8_8_8
    // ::UFLOAT_9_9_9_EXP_5 /* Not of TextureTargetFormat. */
};
constexpr auto formats_class_16 = {
    gpu::TextureFormat::SFLOAT_16,
    gpu::TextureFormat::UINT_8_8,
    gpu::TextureFormat::UINT_16,
    gpu::TextureFormat::SINT_8_8,
    gpu::TextureFormat::SINT_16,
    gpu::TextureFormat::UNORM_8_8,
    gpu::TextureFormat::UNORM_16,
    /* ::SNORM_8_8, */ /* Not of TextureTargetFormat. */
    /* ::SNORM_16 */   /* Not of TextureTargetFormat. */
};
constexpr auto formats_class_8 = {
    gpu::TextureFormat::UINT_8, gpu::TextureFormat::SINT_8, gpu::TextureFormat::UNORM_8,
    /* ::SNORM_8 */ /* Not of TextureTargetFormat. */
};

/* Test texture data. */
constexpr uint texture_size_w = 4;
constexpr uint texture_size_h = 4;
constexpr uint texture_size = texture_size_w * texture_size_h;
constexpr auto texture_usage = GPU_TEXTURE_USAGE_GENERAL;

/* Create a exture of specified format, bind to a temporary FBO and clear to black. */
static gpu::Texture *create_base_texture(TextureFormat format)
{
  gpu::Texture *base = GPU_texture_create_2d(
      "base", texture_size_w, texture_size_h, 1, format, texture_usage, nullptr);
  GPU_texture_mipmap_mode(base, false, false);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);

  gpu::FrameBuffer *fbo = nullptr;
  GPU_framebuffer_ensure_config(&fbo, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(base)});
  GPU_framebuffer_bind(fbo);

  float4 black(0.0f, 0.0f, 0.0f, 0.0f);
  GPU_framebuffer_clear(fbo, GPUFrameBufferBits::GPU_COLOR_BIT, black, 0.0f, 0u);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  GPU_framebuffer_free(fbo);

  return base;
}

/* Create a view texture over base, of format s.t it potentially aliases.  */
static gpu::Texture *create_view_texture(TextureFormat format, gpu::Texture *base)
{
  gpu::Texture *view = GPU_texture_create_view("view", base, format, 0, 1, 0, 1, false, false);
  GPU_texture_mipmap_mode(view, false, false);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);
  return view;
}

/* Read back a texture, and check if any bytes are non-zero. */
static bool check_texture_not_zero(gpu::Texture *texture)
{
  Vector<std::byte> data(texture_size * to_bytesize(texture->format_get()));
  void *ptr = GPU_texture_read(texture, to_texture_data_format(texture->format_get()), 0);
  std::memcpy(data.data(), ptr, data.size());
  MEM_freeN(ptr);
  return std::count(data.begin(), data.end(), std::byte(0)) != data.size();
}

/* Given a pair of TextureFormat values, create base and view textures and attempt
 * to perform a framebuffer clear over the view texture. */
static testing::AssertionResult test_texture_view_clear(
    std::pair<TextureFormat, TextureFormat> formats)
{
  testing::AssertionResult result = testing::AssertionSuccess();

  gpu::Texture *base = create_base_texture(formats.first);
  gpu::Texture *view = create_view_texture(formats.second, base);

  /* First check; the view texture should be all zeroes. */
  if (check_texture_not_zero(view)) {
    result = testing::AssertionFailure()
             << "test_texture_view_clear, initial state failed with aliasing: "
             << GPU_texture_format_name(formats.first) << " -> "
             << GPU_texture_format_name(formats.second) << '\n';
    GPU_texture_free(view);
    GPU_texture_free(base);
    return result;
  }

  /* Create FBO with view as color attachment 0. */
  gpu::FrameBuffer *fbo = nullptr;
  GPU_framebuffer_ensure_config(&fbo, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(view)});
  GPU_framebuffer_bind(fbo);

  /* Clear FBO to junk data. */
  float4 junk(3.141f, 5.926f, 5.358f, 9.790f);
  GPU_framebuffer_clear(fbo, GPUFrameBufferBits::GPU_COLOR_BIT, junk, 0.0f, 0u);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  /* Second check; the view texture should **not** be all zeroes. */
  if (!check_texture_not_zero(view)) {
    result = testing::AssertionFailure()
             << "test_texture_view_clear, fbo clear failed with aliasing: "
             << GPU_texture_format_name(formats.first) << " -> "
             << GPU_texture_format_name(formats.second) << '\n';
  }

  GPU_framebuffer_free(fbo);
  GPU_texture_free(view);
  GPU_texture_free(base);

  return result;
}

static void test_texture_view_format_aliasing()
{
  if (GPU_backend_get_type() != GPU_BACKEND_OPENGL) {
    GTEST_SKIP() << "Texture view format aliasing is only tested on OpenGL.";
  }
  GPU_render_begin();

  /* Test all specified format lists; note that we ignore 96-bit, 48-bit, 24-bit formats. These
   * are specified as supported by glTextureView(), but most don't support framebuffer attachment
   * or have no equivalent in Metal, and are unused by Blender. */
  for (auto formats :
       {formats_class_128, formats_class_64, formats_class_32, formats_class_16, formats_class_8})
  {
    /* Iterate cartesian product of format lists. */
    for (TextureFormat a : formats) {
      for (TextureFormat b : formats) {
        EXPECT_TRUE(test_texture_view_clear({a, b}));
      }
    }
  }

  GPU_render_end();
}
GPU_TEST(texture_view_format_aliasing)

}  // namespace blender::gpu::tests
