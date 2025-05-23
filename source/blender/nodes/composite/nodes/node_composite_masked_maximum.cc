/* SPDX-FileCopyrightText: 2006 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup cmpnodes
 */

#include "BLI_assert.h"
#include "BLI_math_base.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "GPU_shader.hh"

#include "COM_node_operation.hh"
#include "COM_utilities.hh"

#include "node_composite_util.hh"

/* **************** Masked Maximum ******************** */

namespace blender::nodes::node_composite_masked_maximum_cc {

static void cmp_node_masked_maximum_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Image").default_value(0.5f).compositor_domain_priority(0);
  b.add_input<decl::Float>("X Scale").default_value(1.0f).compositor_domain_priority(1);
  b.add_input<decl::Float>("Y Scale").default_value(1.0f).compositor_domain_priority(2);
  b.add_input<decl::Float>("Falloff").default_value(0.0f).compositor_domain_priority(3);
  b.add_output<decl::Float>("Image");
}

using namespace blender::compositor;

class MaskedMaximumOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input_image = this->get_input("Image");
    Result &output_image = this->get_result("Image");

    if (input_image.is_single_value()) {
      output_image.share_data(input_image);
      return;
    }

    if (this->context().use_gpu()) {
      this->execute_gpu(input_image, output_image);
    }
    else {
      this->execute_cpu(input_image, output_image);
    }
  }

  void execute_gpu(const Result &input_image, Result &output_image)
  {
    GPUShader *shader = context().get_shader("compositor_masked_maximum");
    GPU_shader_bind(shader);

    input_image.bind_as_texture(shader, "input_image_tx");
    const Result &input_x_scale = get_input("X Scale");
    input_x_scale.bind_as_texture(shader, "input_x_scale_tx");
    const Result &input_y_scale = get_input("Y Scale");
    input_y_scale.bind_as_texture(shader, "input_y_scale_ty");
    const Result &input_falloff = get_input("Falloff");
    input_falloff.bind_as_texture(shader, "input_falloff_tx");

    Domain domain = compute_domain();
    output_image.allocate_texture(domain);
    output_image.bind_as_image(shader, "output_img");

    compute_dispatch_threads_at_least(shader, domain.size);

    GPU_shader_unbind();
    input_image.unbind_as_texture();
    input_x_scale.unbind_as_texture();
    input_y_scale.unbind_as_texture();
    input_falloff.unbind_as_texture();
    output_image.unbind_as_image();
  }

  void execute_cpu(const Result &input_image, Result &output_image)
  {
    Domain domain = this->compute_domain();
    output_image.allocate_texture(domain);

    parallel_for(domain.size, [&](const int2 texel) {
      output_image.store_pixel(texel,
                               input_image.load_pixel_zero<float, true>(texel) +
                                   get_input("X Scale").load_pixel_zero<float, true>(texel) +
                                   get_input("Y Scale").load_pixel_zero<float, true>(texel) +
                                   get_input("Falloff").load_pixel_zero<float, true>(texel));
    });
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new MaskedMaximumOperation(context, node);
}

}  // namespace blender::nodes::node_composite_masked_maximum_cc

static void register_node_type_cmp_masked_maximum()
{
  namespace file_ns = blender::nodes::node_composite_masked_maximum_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeMaskedMaximum");
  ntype.ui_name = "Masked Maximum";
  ntype.ui_description = "Masked Maximum";
  ntype.nclass = NODE_CLASS_MATTE;
  ntype.declare = file_ns::cmp_node_masked_maximum_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_masked_maximum)
