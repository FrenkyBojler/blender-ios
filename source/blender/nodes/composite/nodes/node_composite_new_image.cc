/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_new_image_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::IntVector>("Size")
      .dimensions(2)
      .default_value(int2(1024))
      .subtype(PROP_UNSIGNED)
      .min(1);

  b.add_output<decl::Vector>("Uniform Coordinates")
      .dimensions(2)
      .structure_type(StructureType::Dynamic)
      .description(
          "Zero centered coordinates normalizes along the larger dimension for uniform scaling");
  b.add_output<decl::Vector>("Normalized Coordinates")
      .dimensions(2)
      .structure_type(StructureType::Dynamic)
      .description("Normalized coordinates with half pixel offsets");
  b.add_output<decl::IntVector>("Pixel Coordinates")
      .dimensions(2)
      .structure_type(StructureType::Dynamic)
      .description("Integer pixel coordinates");
}

using namespace blender::compositor;

class NewImageOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const int2 size = this->get_input("Size").get_single_value_default<int2>();
    const int2 image_size = math::max(int2(1), size);

    Result &uniform_coordinates_result = this->get_result("Uniform Coordinates");
    if (uniform_coordinates_result.should_compute()) {
      const Result &uniform_coordinates = this->context().cache_manager().image_coordinates.get(
          this->context(), image_size, CoordinatesType::Uniform);
      uniform_coordinates_result.wrap_external(uniform_coordinates);
    }

    Result &normalized_coordinates_result = this->get_result("Normalized Coordinates");
    if (normalized_coordinates_result.should_compute()) {
      const Result &normalized_coordinates = this->context().cache_manager().image_coordinates.get(
          this->context(), image_size, CoordinatesType::Normalized);
      normalized_coordinates_result.wrap_external(normalized_coordinates);
    }

    Result &pixel_coordinates_result = this->get_result("Pixel Coordinates");
    if (pixel_coordinates_result.should_compute()) {
      const Result &pixel_coordinates = this->context().cache_manager().image_coordinates.get(
          this->context(), image_size, CoordinatesType::Pixel);
      pixel_coordinates_result.wrap_external(pixel_coordinates);
    }
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new NewImageOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeNewImage");
  ntype.ui_name = "New Image";
  ntype.ui_description = "Returns the coordinates of the pixels of an image with the given size";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;
  bke::node_type_size_preset(ntype, bke::eNodeSizePreset::Middle);

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_new_image_cc
