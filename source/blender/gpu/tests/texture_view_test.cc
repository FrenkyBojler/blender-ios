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

/* Arrays matching the Internal Formats compatibility table of glTextureView,
 * or at least as far as `GPU_format.hh` specifies formats listed there. This
 * excludes the set of compressed formats (e.g. GL_COMPRESSED_RGBA_BPTC_UNORM). */
constexpr auto formats_class_128 = {
    gpu::TextureFormat::SFLOAT_32_32_32_32,
    gpu::TextureFormat::UINT_32_32_32_32,
    gpu::TextureFormat::SINT_32_32_32_32,
};

/* Unsupported, unused in Blender. */
/* Array<gpu::TextureFormat, 1> formats_class_96 = {
    gpu::TextureFormat::SFLOAT_32_32_32,
    gpu::TextureFormat::UINT_32_32_32,
    gpu::TextureFormat::SINT_32_32_32,
}; */

constexpr auto formats_class_64 = {
    gpu::TextureFormat::SFLOAT_16_16_16_16,
    gpu::TextureFormat::SFLOAT_32_32,
    gpu::TextureFormat::UINT_16_16_16_16,
    gpu::TextureFormat::UINT_32_32,
    gpu::TextureFormat::SINT_16_16_16_16,
    gpu::TextureFormat::SINT_32_32,
    gpu::TextureFormat::UNORM_16_16_16_16,
    gpu::TextureFormat::SNORM_16_16_16_16,
};

/* Unsupported, unused in Blender. */
/* constexpr auto formats_class_48 = {
    gpu::TextureFormat::UNORM_16_16_16,
    gpu::TextureFormat::SNORM_16_16_16,
    gpu::TextureFormat::SFLOAT_16_16_16,
    gpu::TextureFormat::UINT_16_16_16,
    gpu::TextureFormat::SINT_16_16_16,
}; */

constexpr auto formats_class_32 = {
    gpu::TextureFormat::SFLOAT_16_16,
    gpu::TextureFormat::UFLOAT_11_11_10, /* Commonly used. Appears supported. */
    gpu::TextureFormat::SFLOAT_32,
    // gpu::TextureFormat::UINT_10_10_10_2, /* Supported, but unused. Does not appear to function
    // on Intel. */
    gpu::TextureFormat::UINT_8_8_8_8,
    gpu::TextureFormat::UINT_16_16,
    gpu::TextureFormat::UINT_32,
    gpu::TextureFormat::SINT_8_8_8_8,
    gpu::TextureFormat::SINT_16_16,
    gpu::TextureFormat::SINT_32,
    // gpu::TextureFormat::UNORM_10_10_10_2, /* Unsupported in OpenGL. */
    gpu::TextureFormat::UNORM_8_8_8_8,
    gpu::TextureFormat::UNORM_16_16,
    gpu::TextureFormat::SNORM_8_8_8_8,
    gpu::TextureFormat::SNORM_16_16,
    gpu::TextureFormat::SRGBA_8_8_8_8
    /* , gpu::TextureFormat::UFLOAT_9_9_9_EXP_5 */ /* Shared exponent does not support FBOs */};

/* Unsupported, unused in Blender. */
/* constexpr auto formats_class_24 = {gpu::TextureFormat::UNORM_8_8_8,
                                                 gpu::TextureFormat::SNORM_8_8_8,
                                                 gpu::TextureFormat::SRGBA_8_8_8,
                                                 gpu::TextureFormat::UINT_8_8_8,
                                                 gpu::TextureFormat::SINT_8_8_8}; */

constexpr auto formats_class_16 = {gpu::TextureFormat::SFLOAT_16,
                                   gpu::TextureFormat::UINT_8_8,
                                   gpu::TextureFormat::UINT_16,
                                   gpu::TextureFormat::SINT_8_8,
                                   gpu::TextureFormat::SINT_16,
                                   gpu::TextureFormat::UNORM_8_8,
                                   gpu::TextureFormat::UNORM_16,
                                   gpu::TextureFormat::SNORM_8_8,
                                   gpu::TextureFormat::SNORM_16};

constexpr auto formats_class_8 = {gpu::TextureFormat::UINT_8,
                                  gpu::TextureFormat::SINT_8,
                                  gpu::TextureFormat::UNORM_8,
                                  gpu::TextureFormat::SNORM_8};

/* Create base texture. Disable filtering for texture completeness. Clear to black,
 * though this is guaranteed by standard. */
gpu::Texture *create_base_texture(TextureFormat format)
{
  gpu::Texture *base = GPU_texture_create_2d(
      "base", 4, 4, 1, format, GPU_TEXTURE_USAGE_GENERAL, nullptr);
  GPU_texture_mipmap_mode(base, false, false);
  uint4 clear_data_zero(0, 0, 0, 0);
  GPU_texture_clear(base, to_texture_data_format(format), clear_data_zero);
  return base;
}

/* Create view texture, aliasing over base texture. */
gpu::Texture *create_view_texture(TextureFormat format, gpu::Texture *base)
{
  gpu::Texture *view = GPU_texture_create_view("view", base, format, 0, 1, 0, 1, false, false);
  GPU_texture_mipmap_mode(view, false, false);
  return view;
}

/* Validate whether a texture is non-zero. */
static bool test_texture_readback(gpu::Texture *texture, TextureFormat format)
{
  void *data_readback = GPU_texture_read(texture, to_texture_data_format(format), 0);
  Vector<std::byte> output(to_bytesize(format));
  std::memcpy(output.data(), data_readback, output.size());

  // Span<std::byte> output_span(data_readback, 4);
}

static bool test_base_texture_clear(std::pair<TextureFormat, TextureFormat> formats) {}
static bool test_view_texture_clear(std::pair<TextureFormat, TextureFormat> formats) {}

static bool test_fbo_clear(std::pair<TextureFormat, TextureFormat> formats)
{
  gpu::Texture *base = create_base_texture(formats.first);
  gpu::Texture *view = create_view_texture(formats.second, base);

  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER);

  /* Create FBO with view as color attachment 0. */
  gpu::FrameBuffer *fbo = nullptr;
  GPU_framebuffer_ensure_config(&fbo, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(view)});
  GPU_framebuffer_bind(fbo);

  /* Perform FBO clear to arbitrary non-zero data. */
  float4 data_arbitrary(3.141f, 5.926f, 5.358f, 9.790f);
  GPU_framebuffer_clear(fbo, GPUFrameBufferBits::GPU_COLOR_BIT, data_arbitrary, 0.0f, 0u);

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);

  /* Perform readback from view. Format does not matter. */
  void *data_readback = GPU_texture_read(view, to_texture_data_format(formats.second), 0);

  GPU_framebuffer_free(fbo);
  GPU_texture_free(view);
  GPU_texture_free(base);
}

static void apply_texture_view_2d_formats(gpu::TextureFormat format_a, gpu::TextureFormat format_b)
{
  auto usage = GPU_TEXTURE_USAGE_GENERAL;

  /* Disable filtering on texture creation; otherwise integer textures and views are incomplete. */
  gpu::Texture *base = GPU_texture_create_2d("base", 4, 4, 1, format_a, usage, nullptr);
  GPU_texture_mipmap_mode(base, false, false);

  /* Clear texture to black, though this should be the case. */
  uint4 clear_data(0, 0, 0, 0);
  GPU_texture_clear(base, to_texture_data_format(format_a), clear_data);

  /* Create view, aliasing the texture. Likewise, disable filtering. */
  gpu::Texture *view = GPU_texture_create_view("view", base, format_b, 0, 1, 0, 1, false, false);
  GPU_texture_mipmap_mode(view, false, false);

  /* Create frame buffer with view as color attachment. */
  gpu::FrameBuffer *fbo = nullptr;
  GPU_framebuffer_ensure_config(&fbo, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(view)});
  GPU_framebuffer_bind(fbo);

  /* Clear view to arbitrary, non-zero data. */
  float4 input_data(13.65f, 0.325f, 0.235f, 0.325f);
  GPU_framebuffer_clear(fbo, GPUFrameBufferBits::GPU_COLOR_BIT, input_data, 0.0f, 0u);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_UPDATE);

  /* Read back texture. Use aliasing to check output, which should not be zero. */
  void *output_data = GPU_texture_read(view, to_texture_data_format(format_b), 0);
  Vector<std::byte> input(to_bytesize(format_b));
  Vector<std::byte> output(to_bytesize(format_b));
  input.fill(std::byte(0));
  std::memcpy(output.data(), output_data, output.size());

  /* Was the FBO modified? */
  bool is_supported = !std::equal(input.begin(), input.end(), output.begin(), output.end());
  if (!is_supported) {
    std::printf(
        "FAIL: %s -> %s\n", GPU_texture_format_name(format_a), GPU_texture_format_name(format_b));
  }
  else {
    std::printf(
        "PASS: %s -> %s\n", GPU_texture_format_name(format_a), GPU_texture_format_name(format_b));
  }

  // GPU_texture_format_name()

  // EXPECT_FALSE(std::equal(input.begin(), input.end(), output.begin(), output.end()));

  MEM_freeN(output_data);
  GPU_framebuffer_free(fbo);
  GPU_texture_free(view);
  GPU_texture_free(base);
}

static void apply_texture_view_2d_formats(std::initializer_list<gpu::TextureFormat> format_list)
{
  for (auto format_a : format_list) {
    for (auto format_b : format_list) {
      apply_texture_view_2d_formats(format_a, format_b);
    }
  }
}

static void test_texture_view_2d_format_aliasing()
{
  if (GPU_backend_get_type() != GPU_BACKEND_OPENGL) {
    GTEST_SKIP() << "Texture view format aliasing is only used on OpeNGL";
  }
  GPU_render_begin();
  apply_texture_view_2d_formats(formats_class_128);

  /* UNSUPPORTED */
  // apply_texture_view_2d_formats<>(formats_class_96);

  apply_texture_view_2d_formats(formats_class_64);

  /* UNSUPPORTED */
  // apply_texture_view_2d_formats<>(formats_class_48);

  apply_texture_view_2d_formats(formats_class_32);

  /* UNSUPPORTED */
  // apply_texture_view_2d_formats<>(formats_class_24);

  apply_texture_view_2d_formats(formats_class_16);
  apply_texture_view_2d_formats(formats_class_8);
  GPU_render_end();
}
GPU_TEST(texture_view_2d_format_aliasing)

}  // namespace blender::gpu::tests
