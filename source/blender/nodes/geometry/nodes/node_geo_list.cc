/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_list_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_output(type, "List").structure_type(StructureType::List);
  }

  b.add_input<decl::Int>("Count").default_value(1).min(1).description(
      "The number of elements in the list");

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input(type, "Value").field_on_all();
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

class ListFieldContext : public FieldContext {
 public:
  ListFieldContext() = default;

  GVArray get_varray_for_input(const FieldInput &field_input,
                               const IndexMask &mask,
                               ResourceScope & /*scope*/) const override
  {
    const bke::IDAttributeFieldInput *id_field_input =
        dynamic_cast<const bke::IDAttributeFieldInput *>(&field_input);

    const fn::IndexFieldInput *index_field_input = dynamic_cast<const fn::IndexFieldInput *>(
        &field_input);

    if (id_field_input == nullptr && index_field_input == nullptr) {
      return {};
    }

    return fn::IndexFieldInput::get_index_varray(mask);
  }
};

static void node_geo_exec(GeoNodeExecParams params)
{
  const int count = params.extract_input<int>("Count");
  if (count <= 0) {
    params.error_message_add(NodeWarningType::Error, "Count must be greater than 0");
    params.set_default_remaining_outputs();
    return;
  }

  GField field = params.extract_input<GField>("Value");
  const CPPType &cpp_type = field.cpp_type();

  nodes::ArrayData array_data = nodes::ArrayData::ForConstructed(cpp_type, count);
  GMutableSpan span(cpp_type, array_data.data, count);

  ListFieldContext context{};
  fn::FieldEvaluator evaluator{context, count};
  evaluator.add_with_destination(std::move(field), span);
  evaluator.evaluate();

  params.set_output("List", nodes::List::create(cpp_type, std::move(array_data), count));
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
      SOCK_GEOMETRY,
      [](bContext * /*C*/, PointerRNA * /*ptr*/, PropertyRNA * /*prop*/, bool *r_free) {
        *r_free = true;
        return enum_items_filter(rna_enum_node_socket_data_type_items,
                                 [](const EnumPropertyItem &item) -> bool {
                                   if (!U.experimental.use_bundle_and_closure_nodes) {
                                     if (ELEM(item.value, SOCK_BUNDLE, SOCK_CLOSURE)) {
                                       return false;
                                     }
                                   }
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
  geo_node_type_base(&ntype, "GeometryNodeList");
  ntype.ui_name = "List";
  ntype.ui_description = "Create a list of values";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_cc
