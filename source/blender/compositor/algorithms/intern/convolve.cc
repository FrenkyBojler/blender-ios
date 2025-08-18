/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <complex>
#include <numeric>

#include "BLI_assert.h"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_fftw.hh"
#include "BLI_index_range.hh"
#include "BLI_task.hh"

#if defined(WITH_FFTW3)
#  include <fftw3.h>
#endif

#include "COM_context.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"

#include "COM_algorithm_convolve.hh"

namespace blender::compositor {

void convolve(Context &context, const Result &input, const Result &kernel, Result &output)
{
  BLI_assert(input.type() == ResultType::Color);
  BLI_assert(kernel.type() == ResultType::Float);
  BLI_assert(output.type() == ResultType::Color);

#if defined(WITH_FFTW3)
  /* Since we will be doing a circular convolution, we need to zero pad the input image by the
   * kernel size and vice versa to avoid the kernel affecting the pixels at the other side of
   * image. The kernel size is limited by the image size since it will have no effect on the image
   * during convolution. */
  const int2 image_size = input.domain().size;
  const int2 kernel_size = kernel.domain().size;
  const int2 needed_padding_amount = math::max(kernel_size, image_size);
  const int2 needed_spatial_size = image_size + needed_padding_amount - 1;
  const int2 spatial_size = fftw::optimal_size_for_real_transform(needed_spatial_size);

  /* The FFTW real to complex transforms utilizes the hermitian symmetry of real transforms and
   * stores only half the output since the other half is redundant, so we only allocate half of
   * the first dimension. See Section 4.3.4 Real-data DFT Array Format in the FFTW manual for
   * more information. */
  const int2 frequency_size = int2(spatial_size.x / 2 + 1, spatial_size.y);

  /* We only process the color channels, the alpha channel is written to the output as is. */
  constexpr int channels_count = 3;
  const int64_t spatial_pixels_per_channel = int64_t(spatial_size.x) * spatial_size.y;
  const int64_t frequency_pixels_per_channel = int64_t(frequency_size.x) * frequency_size.y;
  const int64_t spatial_pixels_count = spatial_pixels_per_channel * channels_count;
  const int64_t frequency_pixels_count = frequency_pixels_per_channel * channels_count;

  float *image_spatial_domain = fftwf_alloc_real(spatial_pixels_count);
  std::complex<float> *image_frequency_domain = reinterpret_cast<std::complex<float> *>(
      fftwf_alloc_complex(frequency_pixels_count));

  /* Create a real to complex plan to transform the image to the frequency domain. */
  fftwf_plan forward_plan = fftwf_plan_dft_r2c_2d(
      spatial_size.y,
      spatial_size.x,
      image_spatial_domain,
      reinterpret_cast<fftwf_complex *>(image_frequency_domain),
      FFTW_ESTIMATE);

  Result input_cpu = context.use_gpu() ? input.download_to_cpu() : input;

  /* Zero pad the image to the required spatial domain size, storing each channel in planar
   * format for better cache locality, that is, RRRR...GGGG...BBBB. */
  threading::memory_bandwidth_bound_task(spatial_pixels_count * sizeof(float), [&]() {
    parallel_for(spatial_size, [&](const int2 texel) {
      const float4 pixel_color = input_cpu.load_pixel_zero<float4>(texel);
      for (const int channel : IndexRange(channels_count)) {
        const int64_t base_index = texel.y * spatial_size.x + texel.x;
        const int64_t output_index = base_index + spatial_pixels_per_channel * channel;
        image_spatial_domain[output_index] = pixel_color[channel];
      }
    });
  });

  threading::parallel_for(IndexRange(channels_count), 1, [&](const IndexRange sub_range) {
    for (const int64_t channel : sub_range) {
      fftwf_execute_dft_r2c(forward_plan,
                            image_spatial_domain + spatial_pixels_per_channel * channel,
                            reinterpret_cast<fftwf_complex *>(image_frequency_domain) +
                                frequency_pixels_per_channel * channel);
    }
  });

  float *kernel_spatial_domain = fftwf_alloc_real(spatial_size.x * spatial_size.y);
  std::complex<float> *kernel_frequency_domain = reinterpret_cast<std::complex<float> *>(
      fftwf_alloc_complex(frequency_pixels_per_channel));

  /* Use a double to sum the kernel since floats are not stable with threaded summation. */
  threading::EnumerableThreadSpecific<double> sum_by_thread([]() { return 0.0; });

  Result kernel_cpu = context.use_gpu() ? kernel.download_to_cpu() : kernel;

  /* Compute the kernel while zero padding to match the spatial size. */
  parallel_for(spatial_size, [&](const int2 texel) {
    const int2 kernel_center = kernel_size / 2;
    const int2 kernel_texel = kernel_center - texel;

    /* We offset the computed kernel with wrap around such that it is centered at the zero
     * point, which is the expected format for doing circular convolutions in the frequency
     * domain. */
    int64_t input_x = mod_i(kernel_texel.x, spatial_size.x);
    int64_t input_y = mod_i(kernel_texel.y, spatial_size.y);
    const int2 texelll = int2(input_x, input_y);

    const float kernel_value = kernel_cpu.load_pixel_zero<float>(texelll);
    kernel_spatial_domain[texel.x + texel.y * spatial_size.x] = kernel_value;
    sum_by_thread.local() += kernel_value;
  });

  fftwf_execute_dft_r2c(forward_plan,
                        kernel_spatial_domain,
                        reinterpret_cast<fftwf_complex *>(kernel_frequency_domain));

  /* The computed kernel is not normalized and should be normalized, but instead of normalizing the
   * kernel during computation, we normalize it in the frequency domain when convolving the kernel
   * to the image since we will be doing sample normalization anyways. This is okay since the
   * Fourier transform is linear. */
  const float normalization_factor = float(
      std::accumulate(sum_by_thread.begin(), sum_by_thread.end(), 0.0));

  /* Multiply the kernel and the image in the frequency domain to perform the convolution. The
   * FFT is not normalized, meaning the result of the FFT followed by an inverse FFT will result
   * in an image that is scaled by a factor of the product of the width and height, so we take
   * that into account by dividing by that scale. See Section 4.8.6 Multi-dimensional Transforms
   * of the FFTW manual for more information. */
  const float normalization_scale = float(spatial_size.x) * spatial_size.y * normalization_factor;
  threading::parallel_for(IndexRange(frequency_size.y), 1, [&](const IndexRange sub_y_range) {
    for (const int64_t channel : IndexRange(channels_count)) {
      for (const int64_t y : sub_y_range) {
        for (const int64_t x : IndexRange(frequency_size.x)) {
          const int64_t base_index = x + y * frequency_size.x;
          const int64_t output_index = base_index + frequency_pixels_per_channel * channel;
          const std::complex<float> kernel_value = kernel_frequency_domain[base_index];
          image_frequency_domain[output_index] *= kernel_value / normalization_scale;
        }
      }
    }
  });

  /* Create a complex to real plan to transform the image to the real domain. */
  fftwf_plan backward_plan = fftwf_plan_dft_c2r_2d(
      spatial_size.y,
      spatial_size.x,
      reinterpret_cast<fftwf_complex *>(image_frequency_domain),
      image_spatial_domain,
      FFTW_ESTIMATE);

  threading::parallel_for(IndexRange(channels_count), 1, [&](const IndexRange sub_range) {
    for (const int64_t channel : sub_range) {
      fftwf_execute_dft_c2r(backward_plan,
                            reinterpret_cast<fftwf_complex *>(image_frequency_domain) +
                                frequency_pixels_per_channel * channel,
                            image_spatial_domain + spatial_pixels_per_channel * channel);
    }
  });

  Result output_cpu = context.create_result(input.type());
  output_cpu.allocate_texture(input.domain(), true, ResultStorageType::CPU);

  /* Copy the result to the output. */
  threading::memory_bandwidth_bound_task(input.size_in_bytes(), [&]() {
    parallel_for(image_size, [&](const int2 texel) {
      float4 color = float4(0.0f);
      for (const int channel : IndexRange(channels_count)) {
        const int64_t base_index = texel.x + texel.y * spatial_size.x;
        const int64_t input_index = base_index + spatial_pixels_per_channel * channel;
        color[channel] = image_spatial_domain[input_index];
      }
      color.w = input_cpu.load_pixel<float4>(texel).w;
      output_cpu.store_pixel(texel, color);
    });
  });

  if (context.use_gpu()) {
    input_cpu.release();
    kernel_cpu.release();
    output = output_cpu.upload_to_gpu(true);
    output_cpu.release();
  }
  else {
    output.steal_data(output_cpu);
  }

  fftwf_destroy_plan(forward_plan);
  fftwf_destroy_plan(backward_plan);
  fftwf_free(image_spatial_domain);
  fftwf_free(image_frequency_domain);
  fftwf_free(kernel_spatial_domain);
  fftwf_free(kernel_frequency_domain);
#else
  output.allocate_texture(input.domain());
  if (context.use_gpu()) {
    GPU_texture_copy(output, input);
  }
  else {
    parallel_for(output.domain().size, [&](const int2 texel) {
      output.store_pixel(texel, input.load_pixel<float4>(texel));
    });
  }
#endif
}

}  // namespace blender::compositor
