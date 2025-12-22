/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_math_vector.hh"

#include "COM_context.hh"
#include "COM_utilities.hh"

#include "COM_algorithm_dual_kawase_blur.hh"
#include "COM_algorithm_symmetric_separable_blur.hh"

namespace blender::compositor {

Result kawase_downsample(
    Context &context, const Result &input, const int2 full_size, int divisor, float ratio)
{
  Result output = context.create_result(ResultType::Color);
  const int2 size = math::max(full_size / divisor, int2(1));
  output.allocate_texture(size);
  const float2 texel_mul = float2(1.0f) / float2(size);
  const float2 texel_add = float2(0.5f) / float2(size);
  const float2 step = float2(ratio) / float2(size);
  parallel_for(size, [&](const int2 texel) {
    /* Each invocation corresponds to output pixel. */
    float2 uv = float2(texel) * texel_mul + texel_add;
    float4 col0 = float4(input.sample_bilinear_extended<Color>(uv));
    float4 col1 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(-0.5f, -0.5f)));
    float4 col2 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(-0.5f, +0.5f)));
    float4 col3 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(+0.5f, -0.5f)));
    float4 col4 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(+0.5f, +0.5f)));
    float4 col = (4.0f * col0 + col1 + col2 + col3 + col4) * 0.125f;
    output.store_pixel(texel, Color(col));
  });
  return output;
}

Result kawase_upsample(
    Context &context, const Result &input, const int2 full_size, int divisor, float ratio)
{
  Result output = context.create_result(ResultType::Color);
  const int2 size = math::max(full_size / divisor, int2(1));
  output.allocate_texture(size);
  const float2 texel_mul = float2(1.0f) / float2(size);
  const float2 texel_add = float2(0.5f) / float2(size);
  const float2 step = ratio / float2(input.domain().data_size);
  parallel_for(size, [&](const int2 texel) {
    /* Each invocation corresponds to output pixel. */
    float2 uv = float2(texel) * texel_mul + texel_add;

    float4 col0 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(0, -1)));
    float4 col1 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(0, +1)));
    float4 col2 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(-1, 0)));
    float4 col3 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(+1, 0)));

    float4 col4 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(-0.5f, -0.5f)));
    float4 col5 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(-0.5f, +0.5f)));
    float4 col6 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(+0.5f, -0.5f)));
    float4 col7 = float4(input.sample_bilinear_extended<Color>(uv + step * float2(+0.5f, +0.5f)));

    float4 col = (col0 + col1 + col2 + col3 + (col4 + col5 + col6 + col7) * 2.0f) * (1.0f / 12.0f);
    output.store_pixel(texel, Color(col));
  });
  return output;
}

void dual_kawase_blur(Context &context, const Result &input, Result &output, float radius)
{
  const int2 full_size = input.domain().data_size;
  Result curr = input;
  bool curr_is_input = true;

  if (radius < 9.0f) {
    symmetric_separable_blur(context, input, output, float2(radius, radius));
    return;
  }

  SCOPED_TIMER(__func__);
  radius *= 1.0f / 3.0f;

  /* Downsample. */
  int last_pass = 1;
  for (int i = 2; i <= radius; i *= 2) {
    Result tmp = kawase_downsample(context, curr, full_size, i, 1.0f);
    if (!curr_is_input) {
      curr.release();
    }
    curr = tmp;
    curr_is_input = false;
    last_pass = i;
  }

  float residual = radius - last_pass;
  if (residual > 0.0f) {
    int next_pass = last_pass * 2;
    float ratio = residual / (next_pass - last_pass);

    /* Downsample and upsample one more step. */
    Result extra_down = kawase_downsample(
        context, curr, full_size, next_pass, 0.5f + 0.5f * ratio);
    Result extra_up = kawase_upsample(
        context, extra_down, full_size, last_pass, 0.5f + 0.5f * ratio);
    extra_down.release();

    /* Mix current with that extra step based on ratio */
    const int2 mix_size = extra_up.domain().data_size;
    parallel_for(mix_size, [&](const int2 texel) {
      float4 pix_curr = float4(curr.load_pixel<Color>(texel));
      float4 pix_next = float4(extra_up.load_pixel<Color>(texel));
      float4 pix = math::interpolate(pix_curr, pix_next, ratio);
      extra_up.store_pixel(texel, Color(pix));
    });
    if (!curr_is_input) {
      curr.release();
    }
    curr = extra_up;
    curr_is_input = false;
  }

  /* Upsample. */
  for (int i = last_pass / 2; i >= 1; i /= 2) {
    Result tmp = kawase_upsample(context, curr, full_size, i, 1.0f);
    if (!curr_is_input) {
      curr.release();
    }
    curr = tmp;
    curr_is_input = false;
  }

  if (context.use_gpu()) {
    Result output_gpu = curr.upload_to_gpu(true);
    output.steal_data(output_gpu);
  }
  else {
    output.steal_data(curr);
  }
}

}  // namespace blender::compositor
