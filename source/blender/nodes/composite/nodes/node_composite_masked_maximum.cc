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
  b.add_input<decl::Color>("Image")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_domain_priority(0);
  b.add_input<decl::Float>("Size")
      .default_value(1.0f)
      .min(0.0f)
      .max(1.0f)
      .compositor_domain_priority(1);
  b.add_output<decl::Color>("Image");
}

using namespace blender::compositor;

class MaskedMaximumOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Image");
    Result &output = this->get_result("Image");

    execute_constant_size_cpu(input, output);
  }

  void execute_constant_size_cpu(const Result &input, Result &output)
  {
    Domain domain = this->compute_domain();
    output.allocate_texture(domain);

    parallel_for(domain.size, [&](const int2 texel) {
      float4 input_pixel = input.load_pixel_zero<float4, true>(texel);

      output.store_pixel(
          texel,
          input_pixel +
              float4(get_input("Size").load_pixel_zero<float, true>(texel), 0.0f, 0.0f, 0.0f));
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
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = file_ns::cmp_node_masked_maximum_declare;
  ntype.flag |= NODE_PREVIEW;
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(register_node_type_cmp_masked_maximum)
