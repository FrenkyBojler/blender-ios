/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include "MEM_guardedalloc.h"

#include "BLI_assert.h"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utildefines.h"

#include "DNA_node_types.h"

#include "RNA_enum_types.hh"

#include "GPU_shader.hh"
#include "GPU_texture.hh"

#include "BKE_node.hh"

#include "COM_domain.hh"
#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_displace_cc {

static void cmp_node_displace_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();

  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);

  b.add_input<decl::Color>("Image")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Vector>("Vector")
      .dimensions(2)
      .default_value({1.0f, 1.0f})
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_TRANSLATION)
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("X Scale")
      .default_value(0.0f)
      .min(-1000.0f)
      .max(1000.0f)
      .structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Y Scale")
      .default_value(0.0f)
      .min(-1000.0f)
      .max(1000.0f)
      .structure_type(StructureType::Dynamic);

  PanelDeclarationBuilder &sampling_panel = b.add_panel("Sampling").default_closed(true);
  sampling_panel.add_input<decl::Menu>("Interpolation")
      .default_value(CMP_NODE_INTERPOLATION_BILINEAR)
      .static_items(rna_enum_node_compositor_interpolation_items)
      .description("Interpolation method");
  sampling_panel.add_input<decl::Menu>("Extension X")
      .default_value(CMP_NODE_EXTENSION_MODE_CLIP)
      .static_items(rna_enum_node_compositor_extension_items)
      .description("The extension mode applied to the X axis");
  sampling_panel.add_input<decl::Menu>("Extension Y")
      .default_value(CMP_NODE_EXTENSION_MODE_CLIP)
      .static_items(rna_enum_node_compositor_extension_items)
      .description("The extension mode applied to the Y axis");
}

static void cmp_node_init_displace(bNodeTree * /*ntree*/, bNode *node)
{
  /* Unused, kept for forward compatibility. */
  NodeDisplaceData *data = MEM_callocN<NodeDisplaceData>(__func__);
  node->storage = data;
}

using namespace blender::compositor;

class DisplaceOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    if (this->is_identity()) {
      const Result &input = this->get_input("Image");
      Result &output = this->get_result("Image");
      output.share_data(input);
      return;
    }

    if (this->context().use_gpu()) {
      this->execute_gpu();
    }
    else {
      this->execute_cpu();
    }
  }

  void execute_gpu()
  {
    const math::SamplingOptions options = this->get_options();
    const char *shader_name;
    switch (options.sampler) {
      case math::Sampler::Anisotropic:
        shader_name = "compositor_displace_anisotropic";
        break;
      case math::Sampler::Bspline:
        shader_name = "compositor_displace_bspline";
        break;
      case math::Sampler::Nearest:
        shader_name = "compositor_displace_nearest";
        break;
      default:
        shader_name = "compositor_displace_box";
        break;
    }
    gpu::Shader *shader = context().get_shader(shader_name);
    GPU_shader_bind(shader);

    const Result &input_image = get_input("Image");
    if (options.sampler == math::Sampler::Anisotropic) {
      GPU_texture_anisotropic_filter(input_image, true);
      GPU_texture_mipmap_mode(input_image, true, true);
    }
    else {
      GPU_texture_filter_mode(input_image, false);
    }
    GPU_texture_extend_mode_x(input_image, map_extension_mode_to_extend_mode(options.wrap_x));
    GPU_texture_extend_mode_y(input_image, map_extension_mode_to_extend_mode(options.wrap_y));
    input_image.bind_as_texture(shader, "input_tx");

    const Result &input_displacement = get_input("Vector");
    input_displacement.bind_as_texture(shader, "displacement_tx");
    const Result &input_x_scale = get_input("X Scale");
    input_x_scale.bind_as_texture(shader, "x_scale_tx");
    const Result &input_y_scale = get_input("Y Scale");
    input_y_scale.bind_as_texture(shader, "y_scale_tx");

    const Domain domain = compute_domain();
    Result &output_image = get_result("Image");
    output_image.allocate_texture(domain);
    output_image.bind_as_image(shader, "output_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    input_image.unbind_as_texture();
    input_displacement.unbind_as_texture();
    input_x_scale.unbind_as_texture();
    input_y_scale.unbind_as_texture();
    output_image.unbind_as_image();
    GPU_shader_unbind();
  }

  void execute_cpu()
  {
    const math::SamplingOptions options = this->get_options();

    const Result &image = get_input("Image");
    const Result &input_displacement = get_input("Vector");
    const Result &x_scale = get_input("X Scale");
    const Result &y_scale = get_input("Y Scale");

    const Domain domain = compute_domain();
    Result &output = get_result("Image");
    output.allocate_texture(domain);

    // Same as node_composite_map_uv, this computes 4 pixels at a time in order to share
    // derivatives */
    const int2 size = domain.size;

    /* 2x2 blocks are used, with the differences between them used as the derivatives */
    parallel_for(math::divide_ceil(size, int2(2)), [&](const int2 base_texel) {
      const int x = base_texel.x * 2;
      const int y = base_texel.y * 2;

      // Block might only have one pixel on the top/right. In this case compute the derivative
      // in the opposite direction (fortunatly the sampling does not care about sign)
      const int2 lower_left_texel = int2(x, y);
      const int x_dir = x < size.x ? 1 : -1;
      const int2 lower_right_texel = int2(x + x_dir, y);
      const int y_dir = y < size.y ? 1 : -1;
      const int2 upper_left_texel = int2(x, y + y_dir);
      const int2 upper_right_texel = int2(x + x_dir, y + y_dir);

      const float2 lower_left_uv = compute_coordinates(
          lower_left_texel, input_displacement, x_scale, y_scale);
      const float2 lower_right_uv = compute_coordinates(
          lower_right_texel, input_displacement, x_scale, y_scale);
      const float2 upper_left_uv = compute_coordinates(
          upper_left_texel, input_displacement, x_scale, y_scale);
      const float2 upper_right_uv = compute_coordinates(
          upper_right_texel, input_displacement, x_scale, y_scale);

      /* Compute the partial derivatives using finite difference. */
      const float2 lower_x_gradient = lower_right_uv - lower_left_uv;
      const float2 left_y_gradient = upper_left_uv - lower_left_uv;
      const float2 right_y_gradient = upper_right_uv - lower_right_uv;
      const float2 upper_x_gradient = upper_right_uv - upper_left_uv;

      /* Computes one of the 2x2 pixels given its texel location, coordinates, and gradients. */
      auto compute_pixel = [&](const int2 &texel,
                               const float2 &coordinates,
                               const float2 &x_gradient,
                               const float2 &y_gradient) {
        output.store_pixel(texel, image.sample_area(options, coordinates, x_gradient, y_gradient));
      };

      /* Compute each of the pixels in the 2x2 block, making sure to exempt out of bounds right
       * and upper pixels. */
      compute_pixel(lower_left_texel, lower_left_uv, lower_x_gradient, left_y_gradient);
      if (x_dir > 0) {
        compute_pixel(lower_right_texel, lower_right_uv, lower_x_gradient, right_y_gradient);
      }
      if (y_dir > 0) {
        compute_pixel(upper_left_texel, upper_left_uv, upper_x_gradient, left_y_gradient);
        if (x_dir > 0) {
          compute_pixel(upper_right_texel, upper_right_uv, upper_x_gradient, right_y_gradient);
        }
      }
    });
  }

  float2 compute_coordinates(const int2 &texel,
                             const Result &input_displacement,
                             const Result &x_scale,
                             const Result &y_scale) const
  {
    float2 scale = float2(x_scale.load_pixel_extended<float, true>(texel),
                          y_scale.load_pixel_extended<float, true>(texel));
    return float2(texel) + float2(0.5f) -
           input_displacement.load_pixel_extended<float2, true>(texel) * scale;
  }

  math::SamplingOptions get_options() const
  {
    math::SamplingOptions ret;

    switch (static_cast<CMPNodeInterpolation>(
        this->get_input("Interpolation")
            .get_single_value_default(MenuValue(CMP_NODE_INTERPOLATION_BILINEAR))
            .value))
    {
      case CMP_NODE_INTERPOLATION_ANISOTROPIC:
        ret.sampler = math::Sampler::Anisotropic;
        break;
      case CMP_NODE_INTERPOLATION_NEAREST:
        ret.sampler = math::Sampler::Nearest;
        break;
      default:  // CMP_NODE_INTERPOLATION_BILINEAR
        ret.sampler = math::Sampler::Box;
        break;
      case CMP_NODE_INTERPOLATION_BICUBIC:
        ret.sampler = math::Sampler::Bspline;
        break;
    }

    switch (static_cast<CMPExtensionMode>(
        this->get_input("Extension X")
            .get_single_value_default(MenuValue(CMP_NODE_EXTENSION_MODE_CLIP))
            .value))
    {
      default:  // case CMP_NODE_EXTENSION_MODE_CLIP:
        ret.wrap_x = math::InterpWrapMode::Border;
        break;
      case CMP_NODE_EXTENSION_MODE_REPEAT:
        ret.wrap_x = math::InterpWrapMode::Repeat;
        break;
      case CMP_NODE_EXTENSION_MODE_EXTEND:
        ret.wrap_x = math::InterpWrapMode::Extend;
        break;
    }

    switch (static_cast<CMPExtensionMode>(
        this->get_input("Extension Y")
            .get_single_value_default(MenuValue(CMP_NODE_EXTENSION_MODE_CLIP))
            .value))
    {
      default:  // case CMP_NODE_EXTENSION_MODE_CLIP:
        ret.wrap_y = math::InterpWrapMode::Border;
        break;
      case CMP_NODE_EXTENSION_MODE_REPEAT:
        ret.wrap_y = math::InterpWrapMode::Repeat;
        break;
      case CMP_NODE_EXTENSION_MODE_EXTEND:
        ret.wrap_y = math::InterpWrapMode::Extend;
        break;
    }

    return ret;
  }

  bool is_identity()
  {
    const Result &input_image = get_input("Image");
    if (input_image.is_single_value()) {
      return true;
    }

    const Result &input_displacement = get_input("Vector");
    if (input_displacement.is_single_value() &&
        math::is_zero(input_displacement.get_single_value<float2>()))
    {
      return true;
    }

    const Result &input_x_scale = get_input("X Scale");
    const Result &input_y_scale = get_input("Y Scale");
    if (input_x_scale.is_single_value() && input_x_scale.get_single_value<float>() == 0.0f &&
        input_y_scale.is_single_value() && input_y_scale.get_single_value<float>() == 0.0f)
    {
      return true;
    }

    return false;
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new DisplaceOperation(context, node);
}

}  // namespace blender::nodes::node_composite_displace_cc

static void register_node_type_cmp_displace()
{
  namespace file_ns = blender::nodes::node_composite_displace_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeDisplace", CMP_NODE_DISPLACE);
  ntype.ui_name = "Displace";
  ntype.ui_description = "Displace pixel position using an offset vector";
  ntype.enum_name_legacy = "DISPLACE";
  ntype.nclass = NODE_CLASS_DISTORT;
  ntype.declare = file_ns::cmp_node_displace_declare;
  ntype.initfunc = file_ns::cmp_node_init_displace;
  blender::bke::node_type_storage(
      ntype, "NodeDisplaceData", node_free_standard_storage, node_copy_standard_storage);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_displace)
