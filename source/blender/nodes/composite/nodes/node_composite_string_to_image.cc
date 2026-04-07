/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "DNA_vfont_types.h"

#include "BKE_vfont.hh"

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_string_to_image_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("String"_ustr).optional_label();
  b.add_input<decl::Font>("Font"_ustr)
      .default_value_fn(
          [](const bNode & /*node*/) { return id_cast<ID *>(BKE_vfont_builtin_ensure()); })
      .optional_label();
  b.add_input<decl::Float>("Size"_ustr).default_value(256.0f).subtype(PROP_UNSIGNED).min(0.0f);

  b.add_output<decl::Color>("Image"_ustr)
      .structure_type(StructureType::Dynamic)
      .description("The image containing the paragraph of text");
}

using namespace blender::compositor;

class StringToImageOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const std::string string = this->get_input("String").get_single_value_default<std::string>();
    const VFont *font = this->get_input("Font").get_single_value_default<VFont *>();
    const float size = this->get_input("Size").get_single_value_default<float>();

    const Result &string_image = this->context().cache_manager().string_images.get(
        this->context(), string, font, size);

    Result &output = this->get_result("Image");
    output.wrap_external(string_image);
  }
};

static NodeOperation *get_compositor_operation(Context &context, const bNode &node)
{
  return new StringToImageOperation(context, node);
}

static void node_register()
{
  static bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeStringToImage");
  ntype.ui_name = "String To Image";
  ntype.ui_description = "Generates an image containing the given paragraph of text";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = get_compositor_operation;

  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_composite_string_to_image_cc
