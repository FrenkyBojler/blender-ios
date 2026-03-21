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

/* support for non-float types, does nearest sampling only. */
template<typename T>
static void realize_on_domain(const Result &input, Result &output, const float3x3 &transformation)
{
  const RealizationOptions realization_options = input.get_realization_options();
  const int2 input_size = input.domain().data_size;
  const int2 output_size = output.domain().data_size;
  const float2 dPdx(transformation[0].xy() / float2(input_size));
  const float2 dPdy(transformation[1].xy() / float2(input_size));
  const float2 translate(transformation[2].xy() / float2(input_size));
  parallel_for(output_size, [&](const int2 texel) {
    const float2 uv = dPdx * texel.x + dPdy * texel.y + translate;
    T sample = input.sample<T>(uv,
                               realization_options.interpolation,
                               realization_options.extension_x,
                               realization_options.extension_y);
    output.store_pixel(texel, sample);
  });
}

template<math::Sampler sampler>
static void realize_on_domain(math::sampler2D &source,
                              Result &output,
                              const float3x3 &transformation,
                              const float2 &wh)
{
  const float2 dPdx(transformation[0].xy());
  const float2 dPdy(transformation[1].xy());
  const float2 translate(transformation[2].xy());
  parallel_for(output.domain().data_size, [&](const int2 texel) {
    float2 uv = dPdx * texel.x + dPdy * texel.y + translate;
    float4 sample = sample_rect<sampler>(source, uv, wh);
    output.store_pixel(texel, Color(sample));
  });
}

void RealizeOnDomainOperation::realize_on_domain_cpu(const SamplerOptions &options,
                                                     const float3x3 &transformation,
                                                     const float2 &wh)
{
  Result &input = this->get_input();
  Result &output = this->get_result();

  switch (input.type()) {
    case ResultType::Float:
    case ResultType::Float2:
    case ResultType::Float3:
    case ResultType::Float4:
    case ResultType::Color: {
      math::sampler2D source{input.sampler2D()};
      source.wrap_x = options.wrap_x;
      source.wrap_y = options.wrap_y;
      switch (options.sampler) {
        case math::Sampler::Nearest:
          realize_on_domain<math::Sampler::Nearest>(source, output, transformation, wh);
          break;
        case math::Sampler::Bilinear:
          realize_on_domain<math::Sampler::Bilinear>(source, output, transformation, wh);
          break;
        default:  // Sampler::Box
          realize_on_domain<math::Sampler::Box>(source, output, transformation, wh);
          break;
        case math::Sampler::Bspline:
          realize_on_domain<math::Sampler::Bspline>(source, output, transformation, wh);
          break;
      }
      break;
    }
    case ResultType::Int:
      realize_on_domain<int32_t>(input, output, transformation);
      break;
    case ResultType::Int2:
      realize_on_domain<int2>(input, output, transformation);
      break;
    case ResultType::Int3:
      realize_on_domain<int3>(input, output, transformation);
      break;
    case ResultType::Bool:
      realize_on_domain<bool>(input, output, transformation);
      break;
    case ResultType::Float4x4:
      realize_on_domain<float4x4>(input, output, transformation);
      break;
    case ResultType::Menu:
      realize_on_domain<nodes::MenuValue>(input, output, transformation);
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
  }
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
