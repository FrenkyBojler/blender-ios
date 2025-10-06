/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <cstdint>
#include "COM_algorithm_distance_field.hh"
#include "COM_node_operation.hh"
#include "node_composite_util.hh"
namespace blender::nodes::node_composite_distance_field_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Bool>("Signed")
      .default_value(true);
  b.add_input<decl::Bool>("Normalize")
  .default_value(true);

  b.add_input<decl::Float>("Matte").hide_value().structure_type(StructureType::Dynamic);
      //.description();

  b.add_output<decl::Float>("Distance").structure_type(StructureType::Dynamic);
  b.add_output<decl::Vector>("Position").structure_type(StructureType::Dynamic);
}
using namespace blender::compositor;
class DistanceFieldOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;
  void execute() override
  {
    const Result &input = this->get_input("Matte");
    Result &position = this->get_result("Position");
    Result &distance = this->get_result("Distance");
    bool is_signed = get_input("Signed").get_single_value_default(true);
    bool normalize = get_input("Normalize").get_single_value_default(true);

    distance_field(this->context(), input, position, distance, is_signed, normalize);
  }
};
static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new DistanceFieldOperation(context, node);
}
static void node_register()
{
  static blender::bke::bNodeType ntype;
  cmp_node_type_base(&ntype, "CompositorNodeDistanceField");
  ntype.ui_name = "Distance Field";
  ntype.ui_description = "Creates a distance field from a matte";
  ntype.nclass = NODE_CLASS_OP_FILTER;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  blender::bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)
}  // namespace blender::nodes::node_composite_distance_field_cc