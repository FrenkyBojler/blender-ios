/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "COM_algorithm_convolve.hh"
#include "COM_node_operation.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_convolve_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Image").hide_value().structure_type(StructureType::Dynamic);
  b.add_input<decl::Float>("Kernel")
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .compositor_realization_mode(CompositorInputRealizationMode::Transforms);
  b.add_input<decl::Bool>("Normalize Kernel")
      .default_value(true)
      .description("Normalizes the kernel such that it integrates to one");

  b.add_output<decl::Color>("Image").structure_type(StructureType::Dynamic);
}

using namespace blender::compositor;

class ConvolveOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const Result &input = this->get_input("Image");
    const Result &kernel = this->get_input("Kernel");
    Result &output = this->get_result("Image");

    if (input.is_single_value() || kernel.is_single_value()) {
      output.share_data(input);
      return;
    }

    convolve(this->context(), input, kernel, output, this->get_normalize_kernel());
  }

  bool get_normalize_kernel()
  {
    return this->get_input("Normalize Kernel").get_single_value_default(true);
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new ConvolveOperation(context, node);
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeConvolve");
  ntype.ui_name = "Convolve";
  ntype.ui_description = "Convolves an image with a kernel";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;

  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_convolve_cc
