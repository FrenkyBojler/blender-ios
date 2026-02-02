/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <limits>

#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include "GPU_capabilities.hh"
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

/* Derivatives converted to nearest rectangle: */
static inline float2 compute_wh(const float3x3 &matrix)
{
  return float2{hypotf(matrix[0][0], matrix[1][0]), hypotf(matrix[0][1], matrix[1][1])};
}

void RealizeOnDomainOperation::execute()
{
  Result &input = this->get_input();
  blender::math::SamplerOptions options = input.domain().get_sampler_options();
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
  float3x3 inverse_transformation = math::invert(input_transformation) * output_transformation;

  /* compute derivatives of input location */
  float2 wh(compute_wh(inverse_transformation));

  /* See if nearest filter will work.
     Todo: it will for interpolating filters if entire matrix is all 0,+1,-1 or translation is
     integers */
  if (options.sampler == math::Sampler::Box && wh[0] < 1.1f && wh[1] < 1.1f) {
    /* bilinear sampling can be used for box if derivative is near 1 */
    options.sampler = math::Sampler::Bilinear;
  }

  /* Translate from pixel centers rather than pixel corners */
  inverse_transformation = inverse_transformation * math::from_location<float3x3>(float2(0.5f));

  /* Don't make the input image smaller than 2 pixels, to avoid aliasing and moire patterns */
  if (wh.x * 2 > input.domain().data_size.x) {
    inverse_transformation = math::from_scale<float3x3>(
                                 float2(input.domain().data_size.x / (wh.x * 2), 1.0f)) *
                             inverse_transformation;
    wh.x = input.domain().data_size.x / 2;
  }
  if (wh.y * 2 > input.domain().data_size.y) {
    inverse_transformation = math::from_scale<float3x3>(
                                 float2(1.0f, input.domain().data_size.y / (wh.y * 2))) *
                             inverse_transformation;
    wh.y = input.domain().data_size.y / 2;
  }

  this->get_result().allocate_texture(domain);

  if (this->context().use_gpu()) {
    this->realize_on_domain_gpu(domain.data_size, options, inverse_transformation, wh);
  }
  else {
    this->realize_on_domain_cpu(domain.data_size, options, inverse_transformation, wh);
  }
}

void RealizeOnDomainOperation::realize_on_domain_gpu(const int2 &size,
                                                     const math::SamplerOptions &options,
                                                     const float3x3 &inverse_transformation,
                                                     const float2 &wh)
{
  Result &input = this->get_input();

  bool nearest = options.sampler == math::Sampler::Nearest;
  int clip = 0;
  GPUSamplerExtendMode extend_x = map_wrap_mode_to_extend_mode(options.wrap_x);
  if (!nearest && extend_x == GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER) {
    clip = 1;
    extend_x = GPU_SAMPLER_EXTEND_MODE_EXTEND;
  }
  GPUSamplerExtendMode extend_y = map_wrap_mode_to_extend_mode(options.wrap_y);
  if (!nearest && extend_y == GPU_SAMPLER_EXTEND_MODE_CLAMP_TO_BORDER) {
    clip |= 2;
    extend_y = GPU_SAMPLER_EXTEND_MODE_EXTEND;
  }
  bool fast = (nearest || (options.sampler == math::Sampler::Bilinear && !clip));
  bool anisotropic = options.sampler == math::Sampler::Anisotropic;

  const char *shader_name = nullptr;
  switch (input.type()) {
    case ResultType::Float:
    case ResultType::Float2:
    case ResultType::Float3:
    case ResultType::Color:
    case ResultType::Float4:
      if (fast)
        shader_name = "compositor_realize_on_domain_float4";
      else if (anisotropic)
        shader_name = "compositor_realize_on_domain_anisotropic";
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
    case ResultType::Bool:
    case ResultType::Menu:
      fast = nearest = true;
      shader_name = "compositor_realize_on_domain_sint8";
      break;
    case ResultType::String:
      /* Single only types do not support GPU code path. */
      BLI_assert(Result::is_single_value_only_type(this->get_input().type()));
      BLI_assert_unreachable();
      break;
  }

  gpu::Shader *shader = this->context().get_shader(shader_name);
  GPU_shader_bind(shader);

  if (fast || anisotropic) {
    /* The matrix must produce uv coordinates */
    const float3x3 mat = math::from_scale<float3x3>(1.0f / float2(input.domain().data_size)) *
                         inverse_transformation;
    GPU_shader_uniform_mat3_as_mat4(shader, "inverse_matrix", mat.ptr());
  }
  else {
    GPU_shader_uniform_mat3_as_mat4(shader, "inverse_matrix", inverse_transformation.ptr());
    GPU_shader_uniform_2fv(shader, "wh", wh);
  }
  GPU_shader_uniform_1i(shader, "clip", clip);

  if (anisotropic) {
    GPU_texture_mipmap_mode(input, true, true);
    GPU_texture_anisotropic_filter(input, true);
  } else {
    GPU_texture_filter_mode(input, !nearest);
  }
  GPU_texture_extend_mode_x(input, extend_x);
  GPU_texture_extend_mode_y(input, extend_y);
  input.bind_as_texture(shader, "input_tx");

  Result &output = this->get_result();
  output.bind_as_image(shader, "domain_img");

  compute_dispatch_threads_at_least(shader, size);

  input.unbind_as_texture();
  output.unbind_as_image();
  GPU_shader_unbind();
}

/* support for non-float types, does nearest sampling only. */
template<typename T>
static void realize_on_domain(const Result &input,
                              Result &output,
                              const float3x3 &inverse_transformation)
{
  const RealizationOptions realization_options = input.get_realization_options();
  const int2 input_size = input.domain().data_size;
  const int2 output_size = output.domain().data_size;
  const float2 dPdx(inverse_transformation[0].xy() / float2(input_size));
  const float2 dPdy(inverse_transformation[1].xy() / float2(input_size));
  const float2 translate(inverse_transformation[2].xy() / float2(input_size));
  parallel_for(output_size, [&](const int2 texel) {
    const float2 uv = dPdx * texel.x + dPdy * texel.y + translate;
    T sample = input.sample<T>(uv,
                               realization_options.interpolation,
                               realization_options.extension_x,
                               realization_options.extension_y);
    output.store_pixel(texel, sample);
  });
}

void RealizeOnDomainOperation::realize_on_domain_cpu(const int2 &size,
                                                     const math::SamplerOptions &options,
                                                     const float3x3 &inverse_transformation,
                                                     const float2 &wh)
{
  Result &input = this->get_input();
  Result &output = this->get_result();

  switch (input.type()) {
    case ResultType::Float:
    case ResultType::Color:
    case ResultType::Float3:
    case ResultType::Float4:
    case ResultType::Float2:
      break;  // use the floating-point code
    case ResultType::Int:
      realize_on_domain<int32_t>(input, output, inverse_transformation);
      return;
    case ResultType::Int2:
      realize_on_domain<int2>(input, output, inverse_transformation);
      return;
    case ResultType::Bool:
      realize_on_domain<bool>(input, output, inverse_transformation);
      return;
    case ResultType::Menu:
      realize_on_domain<nodes::MenuValue>(input, output, inverse_transformation);
      return;
    case ResultType::String:
      BLI_assert_unreachable();
  }

  const math::SamplerSource source{input.samplerSource(options)};

  const float2 dPdx(inverse_transformation[0].xy());
  const float2 dPdy(inverse_transformation[1].xy());
  const float2 translate(inverse_transformation[2].xy());

  if (source.sampler == math::Sampler::Anisotropic) {
    auto sample_area = math::sample_area(source);
    parallel_for(size, [&](const int2 texel) {
      float2 uv = dPdx * texel.x + dPdy * texel.y + translate;
      float4 sample = sample_area(source, uv, dPdx, dPdy);
      output.store_pixel(texel, Color(sample));
    });
    return;
  }

  // locate the optimized version of sample_rect
  auto sample_rect = math::sample_rect(source);

  parallel_for(size, [&](const int2 texel) {
    float2 uv = dPdx * texel.x + dPdy * texel.y + translate;
    float4 sample = sample_rect(source, uv, wh);
    output.store_pixel(texel, Color(sample));
  });
}

Domain RealizeOnDomainOperation::compute_domain()
{
  return target_domain_;
}

/* If the transformations of the input and output domains are within this tolerance value, then
 * realization shouldn't be needed. */
static constexpr float transformation_tolerance = 10e-6f;

Domain RealizeOnDomainOperation::compute_realized_transformation_domain(
    Context &context, const Domain &domain, const bool realize_translation)
{
  /* If the domain is only infinitesimally rotated or scaled, return a domain with just the
   * translation component if not realizing translation. */
  if (math::is_equal(
          float2x2(domain.transformation), float2x2::identity(), transformation_tolerance))
  {
    if (realize_translation) {
      Domain realized_domain = domain;
      realized_domain.transformation = float3x3::identity();
      return realized_domain;
    }
    Domain realized_domain = domain;
    realized_domain.transformation = math::from_location<float3x3>(
        domain.transformation.location());
    return realized_domain;
  }

  /* Compute the 4 corners of the domain. */
  const int2 size = domain.data_size;
  const float2 lower_left_corner = float2(0.0f);
  const float2 lower_right_corner = float2(size.x, 0.0f);
  const float2 upper_left_corner = float2(0.0f, size.y);
  const float2 upper_right_corner = float2(size);

  /* Eliminate the translation component of the transformation. Translation is ignored since it has
   * no effect on the size of the domain and will be restored later. */
  const float3x3 transformation = float3x3(float2x2(domain.transformation));

  /* Translate the input such that it is centered in the virtual compositing space. */
  const float2 center_translation = -float2(size) / 2.0f;
  const float3x3 centered_transformation = math::translate(transformation, center_translation);

  /* Transform each of the 4 corners of the image by the centered transformation. */
  const float2 transformed_lower_left_corner = math::transform_point(centered_transformation,
                                                                     lower_left_corner);
  const float2 transformed_lower_right_corner = math::transform_point(centered_transformation,
                                                                      lower_right_corner);
  const float2 transformed_upper_left_corner = math::transform_point(centered_transformation,
                                                                     upper_left_corner);
  const float2 transformed_upper_right_corner = math::transform_point(centered_transformation,
                                                                      upper_right_corner);

  /* Compute the lower and upper bounds of the bounding box of the transformed corners. */
  const float2 lower_bound = math::min(
      math::min(transformed_lower_left_corner, transformed_lower_right_corner),
      math::min(transformed_upper_left_corner, transformed_upper_right_corner));
  const float2 upper_bound = math::max(
      math::max(transformed_lower_left_corner, transformed_lower_right_corner),
      math::max(transformed_upper_left_corner, transformed_upper_right_corner));

  /* Round the bounds such that they cover the entire transformed domain, which means flooring for
   * the lower bound and ceiling for the upper bound. */
  const int2 integer_lower_bound = int2(math::floor(lower_bound));
  const int2 integer_upper_bound = int2(math::ceil(upper_bound));

  const int2 new_size = integer_upper_bound - integer_lower_bound;

  /* Make sure the new size is safe by clamping to the hardware limits and an upper bound. */
  const int max_size = context.use_gpu() ? GPU_max_texture_size() : 65536;
  const int2 safe_size = math::clamp(new_size, int2(1), int2(max_size));

  /* Create a domain from the new safe size and just the translation component of the
   * transformation if not realizing translation. */
  if (realize_translation) {
    return Domain(safe_size);
  }
  return Domain(safe_size, math::from_location<float3x3>(domain.transformation.location()));
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
  const Domain realized_target_domain =
      RealizeOnDomainOperation::compute_realized_transformation_domain(
          context, target_domain, should_realize_translation);

  /* The input have an almost identical domain to the realized target domain, so no need to realize
   * it and the operation is not needed. */
  if (Domain::is_equal(input_result.domain(), realized_target_domain, transformation_tolerance)) {
    return nullptr;
  }

  /* Otherwise, realization is needed. */
  return new RealizeOnDomainOperation(context, realized_target_domain, input_descriptor.type);
}

}  // namespace blender::compositor
