/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_geometry_util.hh"

#include "NOD_rna_define.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_enum_types.hh"

#include "COM_node_operation.hh"
#include "COM_result.hh"

namespace blender::nodes::node_geo_forgo_value_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.use_custom_socket_order();
  b.allow_any_socket_order();

  const bNode *node = b.node_or_null();
  if (!node) {
    return;
  }

  const eNodeSocketDatatype data_type = eNodeSocketDatatype(node->custom1);

  b.add_default_layout();
  b.add_input(data_type, "Value").hide_value().structure_type(StructureType::Dynamic);
  b.add_output(data_type, "Value").align_with_previous().structure_type(StructureType::Dynamic);
  b.add_input<decl::Bool>("Keep").default_value(false).structure_type(StructureType::Single);
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->use_property_split_set(true);
  layout->use_property_decorate_set(false);
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

static void node_geo_exec(GeoNodeExecParams params)
{
  SocketValueVariant value = params.extract_input<SocketValueVariant>("Value");
  const bool keep = params.extract_input<bool>("Keep");
  if (keep) {
    params.set_output("Value", std::move(value));
  }
  else {
    params.set_default_remaining_outputs();
  }
}

static void node_init(bNodeTree * /*tree*/, bNode *node)
{
  node->custom1 = SOCK_FLOAT;
}

using namespace blender::compositor;

class ForgoValueOperation : public NodeOperation {
 public:
  using NodeOperation::NodeOperation;

  void execute() override
  {
    const bool keep = this->get_input("Keep").get_single_value<bool>();
    Result &output = this->get_result("Value");
    if (keep) {
      const Result &input = this->get_input("Value");
      output.share_data(input);
    }
    else {
      output.allocate_invalid();
    }
  }
};

static NodeOperation *node_get_compositor_operation(Context &context, DNode node)
{
  return new ForgoValueOperation(context, node);
}

static void node_rna(StructRNA *srna)
{
  RNA_def_node_enum(
      srna,
      "data_type",
      "Data Type",
      "",
      rna_enum_node_socket_data_type_items,
      NOD_inline_enum_accessors(custom1),
      SOCK_FLOAT,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(rna_enum_node_socket_data_type_items,
                                 [](const EnumPropertyItem &item) -> bool {
                                   return ELEM(item.value,
                                               SOCK_FLOAT,
                                               SOCK_INT,
                                               SOCK_BOOLEAN,
                                               SOCK_ROTATION,
                                               SOCK_MATRIX,
                                               SOCK_VECTOR,
                                               SOCK_STRING,
                                               SOCK_RGBA,
                                               SOCK_GEOMETRY,
                                               SOCK_OBJECT,
                                               SOCK_COLLECTION,
                                               SOCK_MATERIAL,
                                               SOCK_IMAGE,
                                               SOCK_MENU,
                                               SOCK_BUNDLE,
                                               SOCK_CLOSURE);
                                 });
      });
}

static void node_register()
{
  static blender::bke::bNodeType ntype;

  geo_cmp_node_type_base(&ntype, "NodeForgoValue");
  ntype.ui_name = "Forgo Value";
  ntype.ui_description = "Either pass through the input value or output the fallback value";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.initfunc = node_init;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.declare = node_declare;
  ntype.get_compositor_operation = node_get_compositor_operation;
  blender::bke::node_register_type(ntype);

  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_forgo_value_cc
