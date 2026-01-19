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
Array<gpu::TextureFormat, 3> formats_class_128 = {
    gpu::TextureFormat::SFLOAT_32_32_32_32,
    gpu::TextureFormat::UINT_32_32_32_32,
    gpu::TextureFormat::SINT_32_32_32_32,
};
// Array<gpu::TextureFormat, 1> formats_class_96 = {
//     // gpu::TextureFormat::SFLOAT_32_32_32,
//     // gpu::TextureFormat::UINT_32_32_32,
//     gpu::TextureFormat::SINT_32_32_32,
// };
Array<gpu::TextureFormat, 8> formats_class_64 = {
    gpu::TextureFormat::SFLOAT_16_16_16_16,
    gpu::TextureFormat::SFLOAT_32_32,
    gpu::TextureFormat::UINT_16_16_16_16,
    gpu::TextureFormat::UINT_32_32,
    gpu::TextureFormat::SINT_16_16_16_16,
    gpu::TextureFormat::SINT_32_32,
    gpu::TextureFormat::UNORM_16_16_16_16,
    gpu::TextureFormat::SNORM_16_16_16_16,
};
Array<gpu::TextureFormat, 5> formats_class_48 = {
    gpu::TextureFormat::UNORM_16_16_16,
    gpu::TextureFormat::SNORM_16_16_16,
    gpu::TextureFormat::SFLOAT_16_16_16,
    gpu::TextureFormat::UINT_16_16_16,
    gpu::TextureFormat::SINT_16_16_16,
};
Array<gpu::TextureFormat, 13> formats_class_32 = {
    gpu::TextureFormat::SFLOAT_16_16,
    // gpu::TextureFormat::UFLOAT_11_11_10,
    gpu::TextureFormat::SFLOAT_32,
    // gpu::TextureFormat::UINT_10_10_10_2,
    gpu::TextureFormat::UINT_8_8_8_8,
    gpu::TextureFormat::UINT_16_16,
    gpu::TextureFormat::UINT_32,
    gpu::TextureFormat::SINT_8_8_8_8,
    gpu::TextureFormat::SINT_16_16,
    gpu::TextureFormat::SINT_32,
    // gpu::TextureFormat::UNORM_10_10_10_2,
    gpu::TextureFormat::UNORM_8_8_8_8,
    gpu::TextureFormat::UNORM_16_16,
    gpu::TextureFormat::SNORM_8_8_8_8,
    gpu::TextureFormat::SNORM_16_16,
    gpu::TextureFormat::SRGBA_8_8_8_8
    /* , gpu::TextureFormat::UFLOAT_9_9_9_EXP_5 */ /* Shared exponent does not support FBOs */};
// Array<gpu::TextureFormat, 5> formats_class_24 = {gpu::TextureFormat::UNORM_8_8_8,
//                                                  gpu::TextureFormat::SNORM_8_8_8,
//                                                  gpu::TextureFormat::SRGBA_8_8_8,
//                                                  gpu::TextureFormat::UINT_8_8_8,
//                                                  gpu::TextureFormat::SINT_8_8_8};
Array<gpu::TextureFormat, 9> formats_class_16 = {gpu::TextureFormat::SFLOAT_16,
                                                 gpu::TextureFormat::UINT_8_8,
                                                 gpu::TextureFormat::UINT_16,
                                                 gpu::TextureFormat::SINT_8_8,
                                                 gpu::TextureFormat::SINT_16,
                                                 gpu::TextureFormat::UNORM_8_8,
                                                 gpu::TextureFormat::UNORM_16,
                                                 gpu::TextureFormat::SNORM_8_8,
                                                 gpu::TextureFormat::SNORM_16};
Array<gpu::TextureFormat, 4> formats_class_8 = {gpu::TextureFormat::UINT_8,
                                                gpu::TextureFormat::SINT_8,
                                                gpu::TextureFormat::UNORM_8,
                                                gpu::TextureFormat::SNORM_8};

static void apply_texture_view_2d_formats(gpu::TextureFormat format_a, gpu::TextureFormat format_b)
{
  auto usage = GPU_TEXTURE_USAGE_GENERAL;

  /* Disable filtering on texture creation; otherwise most integer textures and views
   * are incomplete. */
  gpu::Texture *base = GPU_texture_create_2d("base", 4, 4, 1, format_a, usage, nullptr);
  GPU_texture_mipmap_mode(base, false, false);
  gpu::Texture *view = GPU_texture_create_view("view", base, format_b, 0, 1, 0, 1, false, false);
  GPU_texture_mipmap_mode(view, false, false);

  gpu::FrameBuffer *fbo = nullptr;
  GPU_framebuffer_ensure_config(&fbo, {GPU_ATTACHMENT_NONE, GPU_ATTACHMENT_TEXTURE(view)});
  GPU_framebuffer_bind(fbo);

  float4 input_data(0.235f, 0.325f, 0.235f, 0.325f);
  GPU_framebuffer_clear(fbo, GPUFrameBufferBits::GPU_COLOR_BIT, input_data, 0.0f, 0u);

  GPU_memory_barrier(GPU_BARRIER_TEXTURE_UPDATE);
  GPU_memory_barrier(GPU_BARRIER_FRAMEBUFFER | GPU_BARRIER_TEXTURE_UPDATE);

  void *output_data = GPU_texture_read(view, to_texture_data_format(format_b), 0);

  /* Use aliasing to compare input/output. */
  Vector<std::byte> input(to_bytesize(format_b));
  Vector<std::byte> output(to_bytesize(format_b));
  input.fill(std::byte(0));
  std::memcpy(output.data(), output_data, output.size());

  /* Was the FBO modified? */
  EXPECT_FALSE(std::equal(input.begin(), input.end(), output.begin(), output.end()));

  MEM_freeN(output_data);
  GPU_framebuffer_free(fbo);
  GPU_texture_free(view);
  GPU_texture_free(base);
}

template<int64_t N>
static void apply_texture_view_2d_formats(const Array<gpu::TextureFormat, N> &formats)
{
  for (auto format_a : formats) {
    for (auto format_b : formats) {
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
  apply_texture_view_2d_formats<>(formats_class_128);

  /* UNSUPPORTED */
  // apply_texture_view_2d_formats<>(formats_class_96);

  apply_texture_view_2d_formats<>(formats_class_64);

  /* UNSUPPORTED */
  // apply_texture_view_2d_formats<>(formats_class_48);

  apply_texture_view_2d_formats<>(formats_class_32);

  /* UNSUPPORTED */
  // apply_texture_view_2d_formats<>(formats_class_24);

  apply_texture_view_2d_formats<>(formats_class_16);
  apply_texture_view_2d_formats<>(formats_class_8);
  GPU_render_end();
}
GPU_TEST(texture_view_2d_format_aliasing)

}  // namespace blender::gpu::tests
