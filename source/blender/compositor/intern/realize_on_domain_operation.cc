/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <limits>

#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "COM_context.hh"
#include "COM_domain.hh"
#include "COM_input_descriptor.hh"
#include "COM_result.hh"
#include "COM_utilities.hh"

#include "COM_realize_on_domain_operation.hh"

namespace blender::compositor {

/* ------------------------------------------------------------------------------------------------
 * Realize On Domain Operation
 */

RealizeOnDomainOperation::RealizeOnDomainOperation(Context &context,
                                                   Domain target_domain,
                                                   ResultType type)
    : SimpleOperation(context), target_domain_(target_domain)
{
  InputDescriptor input_descriptor;
  input_descriptor.type = type;
  this->declare_input_descriptor(input_descriptor);
  this->populate_result(context.create_result(type));
}

/* This does not save much time here since it is only calculated once. But use the
 * same approximation that the per-pixel sample code will use.
 */
static inline float hypot_fast(float x, float y)
{
  float a = fabsf(x);
  float b = fabsf(y);
  return (a < b) ? b + 0.375f * a : a + 0.375f * b;
}

/* Data passed to the cpu and gpu implementations */
struct RealizeOnDomainOperation::Options {
  Interpolation interpolation;
  Extension extension_mode_x;
  Extension extension_mode_y;
  float3x3 transformation;
  float2 rect;
  bool bilinear;
};

void RealizeOnDomainOperation::execute()
{
  Result &input = this->get_input();
  Options options;
  options.interpolation = input.domain().realization_options.interpolation;
  options.extension_mode_x = input.domain().realization_options.extension_x;
  options.extension_mode_y = input.domain().realization_options.extension_y;
  const Domain domain = this->compute_domain();

  /* a lot of data types do nearest sampling only */
  //if (input.type() <= ResultType::Color)
  //  options.interpolation = Interpolation::Nearest;

  /* Translate the input such that it is centered in the virtual compositing space. */
  float2 input_center_translation = float2(-float2(input.domain().data_size) / 2.0f);

  /* Add any corrective translation if necessary */
  if (options.interpolation == Interpolation::Nearest) {
    /* Bias translations in case of nearest interpolation to avoids the round-to-even behavior of
     * some GPUs at pixel boundaries. */
    input_center_translation += float2(std::numeric_limits<float>::epsilon() * 10e3f);
  }
  else {
    /* Assuming no transformations, if the input size is odd and output size is even or vice versa,
     * the centers of pixels of the input and output will be half a pixel away from each other due
     * to the centering translation. Which introduce fuzzy result due to interpolation. So if one
     * is odd and the other is even, detected by testing the low bit of the xor of the sizes, shift
     * the input by 1/2 pixel so the pixels align. */
    const int2 output_size = domain.data_size;
    const int2 input_size = input.domain().data_size;
    if ((input_size.x ^ output_size.x) & 1)
      input_center_translation.x -= 0.5f;
    if ((input_size.y ^ output_size.y) & 1)
      input_center_translation.y -= 0.5f;
  }

  const float3x3 input_transformation = math::translate(input.domain().transformation,
                                                        input_center_translation);

  /* Translate the output such that it is centered in the virtual compositing space. */
  const float2 output_center_translation = -float2(domain.data_size) / 2.0f;
  const float3x3 output_transformation = math::translate(domain.transformation,
                                                         output_center_translation);

  /* Get the transformation from the output space to the input space */
  options.transformation = math::invert(input_transformation) * output_transformation;

  /* compute derivatives of input location and convert to rectangle */
  float2 wh{hypot_fast(options.transformation[0][0], options.transformation[1][0]),
            hypot_fast(options.transformation[0][1], options.transformation[1][1])};

  /* See if nearest filter will work.
     Todo: it will for interpolating filters if entire matrix is all 0,+1,-1 or translation is
     integers */
  options.bilinear = false;
  if (options.interpolation == Interpolation::Bilinear && wh[0] < 1.1f && wh[1] < 1.1f) {
    /* bilinear sampling can be used for box if derivative is near 1 */
    options.bilinear = true;
  }

  /* Transform from pixel centers rather than pixel corners */
  options.transformation *= math::from_location<float3x3>(float2(0.5f));
  /* Transform to normalized coordinates */
  float2 scale = 1.0f / float2(input.domain().data_size);
  options.transformation = math::from_scale<float3x3>(scale) * options.transformation;
  options.rect = wh * scale;

  /* Don't make the input image smaller than 2 pixels, to avoid aliasing and moire patterns */
  if (options.rect.x > 0.5f) {
    options.transformation = math::from_scale<float3x3>(float2(0.5f / options.rect.x, 1.0f)) * options.transformation;
    options.rect.x = 0.5f;
  }
  if (options.rect.y > 0.5f) {
    options.transformation = math::from_scale<float3x3>(float2(1.0f, 0.5f / options.rect.y)) * options.transformation;
    options.rect.y = 0.5f;
  }

  this->get_result().allocate_texture(domain);

  if (this->context().use_gpu()) {
    this->realize_on_domain_gpu(options);
  }
  else {
    this->realize_on_domain_cpu(options);
  }
}

void RealizeOnDomainOperation::realize_on_domain_gpu(const Options &options)
{
  Result &input = this->get_input();

  const char *shader_name = nullptr;
  switch (input.type()) {
    case ResultType::Float:
    case ResultType::Float2:
    case ResultType::Float3:
    case ResultType::Float4:
    case ResultType::Color:
      if (options.bilinear || options.interpolation == Interpolation::Nearest)
        shader_name = "compositor_realize_on_domain_float4";
      else if (options.interpolation == Interpolation::Bicubic)
        shader_name = "compositor_realize_on_domain_bspline_float4";
      else
        shader_name = "compositor_realize_on_domain_box_float4";
      break;
    case ResultType::Int:
      shader_name = "compositor_realize_on_domain_int";
      break;
    case ResultType::Int2:
      shader_name = "compositor_realize_on_domain_int2";
      break;
    case ResultType::Int3:
      /* Int3 is internally stored in a int4 texture due to GPU module limitations. */
      shader_name = "compositor_realize_on_domain_int4";
      break;
    case ResultType::Bool:
    case ResultType::Menu:
      shader_name = "compositor_realize_on_domain_sint8";
      break;
    case ResultType::Float4x4:
      shader_name = "compositor_realize_on_domain_float4x4";
      break;
    case ResultType::String:
    case ResultType::Object:
    case ResultType::Image:
    case ResultType::Font:
    case ResultType::Scene:
    case ResultType::Text:
    case ResultType::Mask:
      /* Single only types do not support GPU code path. */
      BLI_assert(Result::is_single_value_only_type(this->get_input().type()));
      BLI_assert_unreachable();
      break;
  }

  gpu::Shader *shader = this->context().get_shader(shader_name);
  GPU_shader_bind(shader);

  GPU_shader_uniform_mat3_as_mat4(shader, "transformation", options.transformation.ptr());
  GPU_shader_uniform_2fv(shader, "wh", options.rect);

  /* some filters may want nearest instead of bilinear, fix this */
  GPU_texture_filter_mode(input, options.interpolation != Interpolation::Nearest);
  GPU_texture_extend_mode_x(input, map_extension_mode_to_extend_mode(options.extension_mode_x));
  GPU_texture_extend_mode_y(input, map_extension_mode_to_extend_mode(options.extension_mode_y));
  input.bind_as_texture(shader, "input_tx");

  Result &output = this->get_result();
  output.bind_as_image(shader, "domain_img");

  compute_dispatch_threads_at_least(shader, output.domain().data_size);

  output.unbind_as_image();
  input.unbind_as_texture();
  GPU_shader_unbind();
}

template<typename T>
static void realize_on_domain(const Result &input,
                              Result &output,
                              const Interpolation &interpolation,
                              const Extension &extension_mode_x,
                              const Extension &extension_mode_y,
                              const float3x3 &transformation,
                              const float2 &rect)
{
  const float2 dPdx(transformation[0].xy());
  const float2 dPdy(transformation[1].xy());
  const float2 translate(transformation[2].xy());
  const int2 output_size = output.domain().data_size;
  parallel_for(output_size, [&](const int2 texel) {
    const float2 uv = dPdx * texel.x + dPdy * texel.y + translate;
    T sample = input.sample<T>(uv, interpolation, extension_mode_x, extension_mode_y, rect);
    output.store_pixel(texel, sample);
  });
}

void RealizeOnDomainOperation::realize_on_domain_cpu(const Options &options)
{
  Result &input = this->get_input();
  Result &output = this->get_result();
  input.get_cpp_type()
      .to_static_type<float,
                      float2,
                      float3,
                      float4,
                      Color,
                      int32_t,
                      int2,
                      int3,
                      bool,
                      float4x4,
                      nodes::MenuValue>(
          [&]<typename T>() { realize_on_domain<T>(input, output,
                                                   options.interpolation,
                                                   options.extension_mode_x,
                                                   options.extension_mode_y,
                                                   options.transformation,
                                                   options.rect); });
}

Domain RealizeOnDomainOperation::compute_domain()
{
  return target_domain_;
}

SimpleOperation *RealizeOnDomainOperation::construct_if_needed(
    Context &context,
    const Result &input_result,
    const InputDescriptor &input_descriptor,
    const Domain &operation_domain)
{
  /* This input doesn't need realization, the operation is not needed. */
  if (input_descriptor.realization_mode == InputRealizationMode::None) {
    return nullptr;
  }

  /* The input expects a single value and if no single value is provided, it will be ignored and a
   * default value will be used, so no need to realize it and the operation is not needed. */
  if (input_descriptor.expects_single_value) {
    return nullptr;
  }

  /* Input result is a single value and does not need realization, the operation is not needed. */
  if (input_result.is_single_value()) {
    return nullptr;
  }

  /* If we are realizing on the operation domain, then our target domain is the operation domain,
   * otherwise, we are only realizing the transforms, then our target domain is the input's one. */
  const bool use_operation_domain = input_descriptor.realization_mode ==
                                    InputRealizationMode::OperationDomain;
  const Domain target_domain = use_operation_domain ? operation_domain : input_result.domain();

  const bool should_realize_translation = input_descriptor.realization_mode ==
                                          InputRealizationMode::Transforms;
  const Domain realized_target_domain = target_domain.realize_transformation(
      should_realize_translation);

  /* The input have an almost identical domain to the realized target domain, so no need to realize
   * it and the operation is not needed. */
  if (Domain::is_equal(input_result.domain(), realized_target_domain)) {
    return nullptr;
  }

  return new RealizeOnDomainOperation(context, realized_target_domain, input_descriptor.type);
}

}  // namespace blender::compositor
