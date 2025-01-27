/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "COM_node_operation.hh"

#include "node_composite_util.hh"

namespace blender::nodes::node_composite_set_realization_options_cc {

NODE_STORAGE_FUNCS(NodeSetRealizationOptionsData)

static void cmp_node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Color>("Image")
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .compositor_domain_priority(0)
      .compositor_realization_options(CompositorInputRealizationOptions::None);
  b.add_output<decl::Color>("Image");
}

static void node_composit_init(bNodeTree * /*ntree*/, bNode *node)
{
  NodeSetRealizationOptionsData *data = MEM_cnew<NodeSetRealizationOptionsData>(__func__);
  node->storage = data;

  data->interpolation = CMP_NODE_INTERPOLATION_NEAREST;
  data->repeat_x = false;
  data->repeat_y = false;
}

static void node_composit_draw_buttons(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  uiItemR(layout, ptr, "interpolation", UI_ITEM_R_SPLIT_EMPTY_NAME, "", ICON_NONE);

  uiLayout *row = uiLayoutSplit(layout, 0.6f, true);
  uiItemL(row, IFACE_("Repeat"), ICON_NONE);
  uiItemR(row,
          ptr,
          "use_repeat_x",
          UI_ITEM_R_SPLIT_EMPTY_NAME | UI_ITEM_R_TOGGLE,
          IFACE_("X"),
          ICON_NONE);
  uiItemR(row,
          ptr,
          "use_repeat_y",
          UI_ITEM_R_SPLIT_EMPTY_NAME | UI_ITEM_R_TOGGLE,
          IFACE_("Y"),
          ICON_NONE);
}

using namespace blender::compositor;

class SetRealizationOptionsOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    Result &input = get_input("Image");
    Result &output = get_result("Image");

    input.pass_through(output);
    output.get_realization_options() = RealizationOptions{this->get_interpolation(),
                                                          bool(node_storage(bnode()).repeat_x),
                                                          bool(node_storage(bnode()).repeat_y)};
  }

  Interpolation get_interpolation() const
  {
    switch (node_storage(bnode()).interpolation) {
      case CMP_NODE_INTERPOLATION_NEAREST:
        return Interpolation::Nearest;
      case CMP_NODE_INTERPOLATION_BILINEAR:
        return Interpolation::Bilinear;
      case CMP_NODE_INTERPOLATION_BICUBIC:
        return Interpolation::Bicubic;
    }

    BLI_assert_unreachable();
    return Interpolation::Nearest;
  }
};

static NodeOperation *get_compositor_operation(Context &context, DNode node)
{
  return new SetRealizationOptionsOperation(context, node);
}

}  // namespace blender::nodes::node_composite_set_realization_options_cc

void register_node_type_cmp_set_realization_options()
{
  namespace file_ns = blender::nodes::node_composite_set_realization_options_cc;

  static blender::bke::bNodeType ntype;

  cmp_node_type_base(&ntype, "CompositorNodeSetRealizationOptions");
  ntype.ui_name = "Set Realization Options";
  ntype.ui_description =
      "Sets the realization options used when realizing an image on a different domain";
  ntype.nclass = NODE_CLASS_DISTORT;
  ntype.declare = file_ns::cmp_node_declare;
  ntype.draw_buttons = file_ns::node_composit_draw_buttons;
  ntype.initfunc = file_ns::node_composit_init;
  blender::bke::node_type_storage(&ntype,
                                  "NodeSetRealizationOptionsData",
                                  node_free_standard_storage,
                                  node_copy_standard_storage);
  ntype.get_compositor_operation = file_ns::get_compositor_operation;

  blender::bke::node_register_type(&ntype);
}
