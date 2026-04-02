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

struct RealizeOnDomainOperation::SamplerOptions {
  math::Sampler sampler;
  math::InterpWrapMode wrap_x;
  math::InterpWrapMode wrap_y;
  SamplerOptions(const Domain &domain)
  {
    switch (domain.realization_options.interpolation) {
      case Interpolation::Nearest:
        sampler = math::Sampler::Nearest;
        break;
      default: /* case Interpolation::Bilinear: */
        sampler = math::Sampler::Box;
        break;
      case Interpolation::Bicubic:
        sampler = math::Sampler::Bspline;
        break;
    }
    wrap_x = map_extension_mode_to_wrap_mode(domain.realization_options.extension_x);
    wrap_y = map_extension_mode_to_wrap_mode(domain.realization_options.extension_y);
  }
};

void RealizeOnDomainOperation::execute()
{
  Result &input = this->get_input();
  SamplerOptions options(input.domain());
  const Domain domain = this->compute_domain();

  /* Translate the input such that it is centered in the virtual compositing space. */
  float2 input_center_translation = float2(-float2(input.domain().data_size) / 2.0f);

  /* Add any corrective translation if necessary */
  if (options.sampler == math::Sampler::Nearest) {
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
  float3x3 transformation = math::invert(input_transformation) * output_transformation;

  /* compute derivatives of input location and convert to rectangle */
  float2 wh{hypot_fast(transformation[0][0], transformation[1][0]),
            hypot_fast(transformation[0][1], transformation[1][1])};

  /* See if nearest filter will work.
     Todo: it will for interpolating filters if entire matrix is all 0,+1,-1 or translation is
     integers */
  if (options.sampler == math::Sampler::Box && wh[0] < 1.1f && wh[1] < 1.1f) {
    /* bilinear sampling can be used for box if derivative is near 1 */
    options.sampler = math::Sampler::Bilinear;
  }

  /* Translate from pixel centers rather than pixel corners */
  transformation *= math::from_location<float3x3>(float2(0.5f));

  /* Don't make the input image smaller than 2 pixels, to avoid aliasing and moire patterns */
  if (wh.x * 2 > input.domain().data_size.x) {
    transformation = math::from_scale<float3x3>(
                         float2(input.domain().data_size.x / (wh.x * 2), 1.0f)) *
                     transformation;
    wh.x = input.domain().data_size.x / 2;
  }
  if (wh.y * 2 > input.domain().data_size.y) {
    transformation = math::from_scale<float3x3>(
                         float2(1.0f, input.domain().data_size.y / (wh.y * 2))) *
                     transformation;
    wh.y = input.domain().data_size.y / 2;
  }

  this->get_result().allocate_texture(domain);

  if (this->context().use_gpu()) {
    this->realize_on_domain_gpu(options, transformation, wh);
  }
  else {
    this->realize_on_domain_cpu(options, transformation, wh);
  }
}

void RealizeOnDomainOperation::realize_on_domain_gpu(const SamplerOptions &options,
                                                     const float3x3 &transformation,
                                                     const float2 &wh)
{
  Result &input = this->get_input();

  bool nearest = options.sampler == math::Sampler::Nearest;
  bool fast = (nearest || (options.sampler == math::Sampler::Bilinear));

  const char *shader_name = nullptr;
  switch (input.type()) {
    case ResultType::Float:
    case ResultType::Float2:
    case ResultType::Float3:
    case ResultType::Float4:
    case ResultType::Color:
      if (fast)
        shader_name = "compositor_realize_on_domain_float4";
      else if (options.sampler == math::Sampler::Bspline)
        shader_name = "compositor_realize_on_domain_bspline_float4";
      else if (options.sampler == math::Sampler::Bilinear)
        shader_name = "compositor_realize_on_domain_bilinear_float4";
      else
        shader_name = "compositor_realize_on_domain_box_float4";
      break;
    case ResultType::Int:
      fast = nearest = true;
      shader_name = "compositor_realize_on_domain_int";
      break;
    case ResultType::Int2:
      fast = nearest = true;
      shader_name = "compositor_realize_on_domain_int2";
      break;
    case ResultType::Int3:
      /* Int3 is internally stored in a int4 texture due to GPU module limitations. */
      fast = nearest = true;
      shader_name = "compositor_realize_on_domain_int4";
      break;
    case ResultType::Bool:
    case ResultType::Menu:
      fast = nearest = true;
      shader_name = "compositor_realize_on_domain_sint8";
      break;
    case ResultType::Float4x4:
      fast = nearest = true;
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

  if (fast) {
    /* The matrix must produce uv coordinates */
    const float3x3 mat = math::from_scale<float3x3>(1.0f / float2(input.domain().data_size)) *
                         transformation;
    GPU_shader_uniform_mat3_as_mat4(shader, "transformation", mat.ptr());
  }
  else {
    GPU_shader_uniform_mat3_as_mat4(shader, "transformation", transformation.ptr());
    GPU_shader_uniform_2fv(shader, "wh", wh);
  }

  GPU_texture_filter_mode(input, !nearest);
  GPU_texture_extend_mode_x(input, map_wrap_mode_to_extend_mode(options.wrap_x));
  GPU_texture_extend_mode_y(input, map_wrap_mode_to_extend_mode(options.wrap_y));
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
                              const float3x3 &transformation,
                              const float2 &wh)
{
  const RealizationOptions realization_options = input.get_realization_options();
  const float2 scale(1.0f / float2(input.domain().data_size));
  const float2 dPdx(transformation[0].xy() * scale);
  const float2 dPdy(transformation[1].xy() * scale);
  const float2 translate(transformation[2].xy() * scale);
  const float2 rect(wh * scale);

  const int2 output_size = output.domain().data_size;
  parallel_for(output_size, [&](const int2 texel) {
    const float2 uv = dPdx * texel.x + dPdy * texel.y + translate;
    T sample = input.sample<T>(uv,
                               realization_options.interpolation,
                               realization_options.extension_x,
                               realization_options.extension_y,
                               rect);
    output.store_pixel(texel, sample);
  });
}

void RealizeOnDomainOperation::realize_on_domain_cpu(const SamplerOptions &,
                                                     const float3x3 &transformation,
                                                     const float2 &wh)
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
          [&]<typename T>() { realize_on_domain<T>(input, output, transformation, wh); });
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
