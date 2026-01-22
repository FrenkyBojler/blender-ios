/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "gpu_testing.hh"

#include "MEM_guardedalloc.h"

#include "BLI_array.hh"
#include "BLI_math_half.hh"
#include "BLI_math_vector_types.hh"

#include "GPU_context.hh"
#include "GPU_framebuffer.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "gpu_texture_private.hh"

#include <bitset>

namespace blender::gpu::tests {

/* Create a 1x1px texture of specified format and clear to black. Texture must
 * support readback and view. */
static gpu::Texture *create_base_texture(TextureFormat format)
{
  constexpr eGPUTextureUsage usage = GPU_TEXTURE_USAGE_GENERAL | GPU_TEXTURE_USAGE_HOST_READ |
                                     GPU_TEXTURE_USAGE_FORMAT_VIEW;

  gpu::Texture *base = GPU_texture_create_2d("base", 1, 1, 1, format, usage, nullptr);
  GPU_texture_mipmap_mode(base, false, false);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);

  gpu::FrameBuffer *fbo = nullptr;
  GPU_framebuffer_ensure_config(&fbo, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(base)});
  GPU_framebuffer_bind(fbo);

  float4 zero_color(0.0f, 0.0f, 0.0f, 0.0f);
  GPU_framebuffer_clear(fbo, GPUFrameBufferBits::GPU_COLOR_BIT, zero_color, 0.0f, 0u);
  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  GPU_framebuffer_free(fbo);

  return base;
}

/* Create a view texture over base, of format s.t it potentially aliases.  */
static gpu::Texture *create_view_texture(TextureFormat format, gpu::Texture *base)
{
  gpu::Texture *view = GPU_texture_create_view("view", base, format, 0, 1, 0, 1, false, false);
  GPU_texture_mipmap_mode(view, false, false);
  return view;
}

/* Read back a single-pixel, n-channel texture of type float or half, return float4. */
static float4 get_texture_color(gpu::Texture *texture)
{
  TextureFormat format = GPU_texture_format(texture);

  void *src = GPU_texture_read(texture, GPU_DATA_FLOAT, 0);
  float4 dst(0.0f, 0.0f, 0.0f, 0.0f);
  std::memcpy(dst, src, is_half_float(format) ? 2 * to_bytesize(format) : to_bytesize(format));

  MEM_freeN(src);
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
  EXPECT_EQ(get_texture_color(view), zero_color);

  /* Create FBO with view as color attachment 0. */
  gpu::FrameBuffer *fbo = nullptr;
  GPU_framebuffer_ensure_config(&fbo, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(view)});
  GPU_framebuffer_bind(fbo);

  /* Clear FBO to specific color. */
  float4 test_color(2.0f, 0.25f, 1.25f, 0.25f);
  for (uint i = to_component_len(FormatB); i < 4; ++i) {
    test_color[i] = 0.0f;
  }
  GPU_framebuffer_clear(fbo, GPUFrameBufferBits::GPU_COLOR_BIT, test_color, 0.0f, 0u);

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  /* Second check; the view texture should read back this color. */
  EXPECT_EQ(get_texture_color(view), test_color);

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

// /* Arrays overlapping (A) the internal formats table of glTextureView, and (B)
//  * the supported types in gpu::TextureTargetFormat::. This excludes compressed
//  * formats, depth textures, as these remain unsupported. */
// constexpr auto formats_class_128 = {
//     gpu::TextureFormat::SFLOAT_32_32_32_32,
//     gpu::TextureFormat::UINT_32_32_32_32,
//     gpu::TextureFormat::SINT_32_32_32_32,
// };
// constexpr auto formats_class_64 = {
//     gpu::TextureFormat::SFLOAT_16_16_16_16,
//     gpu::TextureFormat::SFLOAT_32_32,
//     gpu::TextureFormat::UINT_16_16_16_16,
//     gpu::TextureFormat::UINT_32_32,
//     gpu::TextureFormat::SINT_16_16_16_16,
//     gpu::TextureFormat::SINT_32_32,
//     gpu::TextureFormat::UNORM_16_16_16_16,
//     /* ::SNORM_16_16_16_16, */ /* Not of TextureTargetFormat. */
// };
// constexpr auto formats_class_32 = {
//     gpu::TextureFormat::SFLOAT_16_16,
//     gpu::TextureFormat::UFLOAT_11_11_10,
//     gpu::TextureFormat::SFLOAT_32,
//     // gpu::TextureFormat::UINT_10_10_10_2, /* Unused. Breaks on Intel. */
//     gpu::TextureFormat::UINT_8_8_8_8,
//     gpu::TextureFormat::UINT_16_16,
//     gpu::TextureFormat::UINT_32,
//     gpu::TextureFormat::SINT_8_8_8_8,
//     gpu::TextureFormat::SINT_16_16,
//     gpu::TextureFormat::SINT_32,
//     gpu::TextureFormat::UNORM_10_10_10_2,
//     gpu::TextureFormat::UNORM_8_8_8_8,
//     gpu::TextureFormat::UNORM_16_16,
//     /* ::SNORM_8_8_8_8, */ /* Not of TextureTargetFormat. */
//     /* ::SNORM_16_16, */   /* Not of TextureTargetFormat. */
//     gpu::TextureFormat::SRGBA_8_8_8_8
//     // ::UFLOAT_9_9_9_EXP_5 /* Not of TextureTargetFormat. */
// };
// constexpr auto formats_class_16 = {
//     gpu::TextureFormat::SFLOAT_16,
//     gpu::TextureFormat::UINT_8_8,
//     gpu::TextureFormat::UINT_16,
//     gpu::TextureFormat::SINT_8_8,
//     gpu::TextureFormat::SINT_16,
//     gpu::TextureFormat::UNORM_8_8,
//     gpu::TextureFormat::UNORM_16,
//     /* ::SNORM_8_8, */ /* Not of TextureTargetFormat. */
//     /* ::SNORM_16 */   /* Not of TextureTargetFormat. */
// };
// constexpr auto formats_class_8 = {
//     gpu::TextureFormat::UINT_8,
//     gpu::TextureFormat::SINT_8,
//     gpu::TextureFormat::UNORM_8,
//     /* ::SNORM_8 */ /* Not of TextureTargetFormat. */
// };

// static void test_texture_view_format_aliasing()
// {
//   if (GPU_backend_get_type() != GPU_BACKEND_OPENGL) {
//     GTEST_SKIP() << "Texture view format aliasing is only tested on OpenGL.";
//   }
//   GPU_render_begin();

//   /* Test all specified format lists; note that we ignore 96-bit, 48-bit,
//   24-bit
//    * formats. These are specified as supported by glTextureView(), but most
//    * don't support framebuffer attachment or have no equivalent in Metal, and
//    * are unused by Blender. */
//   // for (auto formats :
//   //      {formats_class_128, formats_class_64, formats_class_32,
//   //      formats_class_16, formats_class_8})
//   // {
//   //   /* Iterate cartesian product of format lists. */
//   //   for (TextureFormat a : formats) {
//   //     for (TextureFormat b : formats) {
//   //       EXPECT_TRUE(test_texture_view_clear({a, b}));
//   //     }
//   //   }
//   // }

//   GPU_render_end();
// }
// GPU_TEST(texture_view_format_aliasing)

}  // namespace blender::gpu::tests
