/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "GPU_texture.hh"

#include "gpu_testing.hh"

namespace blender::gpu::tests {

  static void test_pixel_buffer_size()
  {
    GPUPixelBuffer *pixel_buffer = GPU_pixel_buffer_create(1024*1024);
    size_t size = GPU_pixel_buffer_size(pixel_buffer);

    EXPECT_EQ( 1024*1024, size);

    GPU_pixel_buffer_free(pixel_buffer);
  }
  GPU_TEST(pixel_buffer_size);

  static void test_pixel_buffer_extern()
  {
    GPUPixelBuffer *pixel_buffer = GPU_pixel_buffer_create(1024*1024);
    GPUPixelBufferNativeHandle native_handle = GPU_pixel_buffer_get_native_handle(pixel_buffer);

    EXPECT_EQ( 1024*1024, native_handle.size);
    EXPECT_NE(0, native_handle.handle);

    GPU_pixel_buffer_free(pixel_buffer);
  }
  GPU_TEST(pixel_buffer_extern);

}  // namespace blender::gpu::tests
