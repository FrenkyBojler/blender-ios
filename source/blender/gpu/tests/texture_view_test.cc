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

constexpr uint texture_size_w = 4;
constexpr uint texture_size_h = 4;
constexpr uint texture_size = texture_size_w * texture_size_h;

/* Arrays matching the Internal Formats compatibility table of glTextureView,
 * or at least as far as `GPU_format.hh` specifies formats listed there. This
 * excludes the set of compressed formats (e.g. GL_COMPRESSED_RGBA_BPTC_UNORM). */
constexpr auto formats_class_128 = {
    gpu::TextureTargetFormat::SFLOAT_32_32_32_32,
    gpu::TextureTargetFormat::UINT_32_32_32_32,
    gpu::TextureTargetFormat::SINT_32_32_32_32,
};

constexpr auto formats_class_64 = {
    gpu::TextureTargetFormat::SFLOAT_16_16_16_16,
    gpu::TextureTargetFormat::SFLOAT_32_32,
    gpu::TextureTargetFormat::UINT_16_16_16_16,
    gpu::TextureTargetFormat::UINT_32_32,
    gpu::TextureTargetFormat::SINT_16_16_16_16,
    gpu::TextureTargetFormat::SINT_32_32,
    gpu::TextureTargetFormat::UNORM_16_16_16_16,
    /* ::SNORM_16_16_16_16, */ /* Not of TextureTargetFormat. */
};

constexpr auto formats_class_32 = {
    gpu::TextureTargetFormat::SFLOAT_16_16,
    gpu::TextureTargetFormat::UFLOAT_11_11_10, /* Commonly used. Appears supported. So why does it
                                                * break things aliasing from SFLOAT_16_16? */
    gpu::TextureTargetFormat::SFLOAT_32,
    // ::UINT_10_10_10_2, /* Supported, but unused. Breaks on Intel? */
    gpu::TextureTargetFormat::UINT_8_8_8_8,
    gpu::TextureTargetFormat::UINT_16_16,
    gpu::TextureTargetFormat::UINT_32,
    gpu::TextureTargetFormat::SINT_8_8_8_8,
    gpu::TextureTargetFormat::SINT_16_16,
    gpu::TextureTargetFormat::SINT_32,
    // ::UNORM_10_10_10_2, /* Unsupported in OpenGL. */
    gpu::TextureTargetFormat::UNORM_8_8_8_8,
    gpu::TextureTargetFormat::UNORM_16_16,
    /* ::SNORM_8_8_8_8, */ /* Not of TextureTargetFormat. */
    /* ::SNORM_16_16, */   /* Not of TextureTargetFormat. */
    gpu::TextureTargetFormat::SRGBA_8_8_8_8
    // ::UFLOAT_9_9_9_EXP_5 /* Not of TextureTargetFormat. */
};

constexpr auto formats_class_16 = {
    gpu::TextureTargetFormat::SFLOAT_16,
    gpu::TextureTargetFormat::UINT_8_8,
    gpu::TextureTargetFormat::UINT_16,
    gpu::TextureTargetFormat::SINT_8_8,
    gpu::TextureTargetFormat::SINT_16,
    gpu::TextureTargetFormat::UNORM_8_8,
    gpu::TextureTargetFormat::UNORM_16,
    /* ::SNORM_8_8, */ /* Not of TextureTargetFormat. */
    /* ::SNORM_16 */   /* Not of TextureTargetFormat. */
};

constexpr auto formats_class_8 = {
    gpu::TextureTargetFormat::UINT_8,
    gpu::TextureTargetFormat::SINT_8,
    gpu::TextureTargetFormat::UNORM_8,
    /* ::SNORM_8 */ /* Not of TextureTargetFormat. */
};

/* Create a exture of specified format, bind to a temporary FBO and clear to black. */
static gpu::Texture *create_base_texture(TextureFormat format)
{
  gpu::Texture *base = GPU_texture_create_2d(
      "base", texture_size_w, texture_size_h, 1, format, GPU_TEXTURE_USAGE_GENERAL, nullptr);
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
             << "test_view_framebuffer_clear, initial state failed with aliasing: "
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
             << "test_view_framebuffer_clear, fbo clear failed with aliasing: "
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
    GTEST_SKIP() << "Texture view format aliasing is only used on OpeNGL";
  }
  GPU_render_begin();

  /* Test all specified format lists; note that we ignore 96-bit, 48-bit, 24-bit formats
   * given their lack of FBO support or general support in Blender. */
  for (auto formats :
       {formats_class_128, formats_class_64, formats_class_32, formats_class_16, formats_class_8})
  {
    /* Iterate cartesian product of format list. */
    for (TextureTargetFormat a : formats) {
      for (TextureTargetFormat b : formats) {
        EXPECT_TRUE(test_texture_view_clear({to_texture_format(a), to_texture_format(b)}));
      }
    }
  }
  GPU_render_end();
}
GPU_TEST(texture_view_format_aliasing)

}  // namespace blender::gpu::tests
