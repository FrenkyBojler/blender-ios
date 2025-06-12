/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_list_get_element_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.add_input<decl::Int>("Index").min(0).supports_field();

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input(type, "List").structure_type(StructureType::List);
  }

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_output(type, "Value").dependent_field({0});
  }
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

class SampleIndexFunction : public mf::MultiFunction {
  ListPtr list_;

  mf::Signature signature_;

 public:
  SampleIndexFunction(ListPtr list) : list_(std::move(list))
  {
    mf::SignatureBuilder builder{"Sample Index", signature_};
    builder.single_input<int>("Index");
    builder.single_output("Value", list_->cpp_type());
    this->set_signature(&signature_);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<int> &indices = params.readonly_single_input<int>(0, "Index");
    GMutableSpan dst = params.uninitialized_single_output(1, "Value");
    const List::DataVariant &data = list_->data();
    if (const auto *array_data = std::get_if<nodes::ArrayData>(&data)) {
      const GSpan span(list_->cpp_type(), array_data->data, list_->size());
      bke::copy_with_checked_indices(GVArray::ForSpan(span), indices, mask, dst);
    }
    else if (const auto *single_data = std::get_if<nodes::SingleData>(&data)) {
      list_->cpp_type().fill_construct_indices(single_data->value, dst.data(), mask);
    }
  }
};

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

static void node_geo_exec(GeoNodeExecParams params)
{
  Field<int> index = params.extract_input<Field<int>>("Index");
  ListPtr list = params.extract_input<ListPtr>("List");
  if (!list) {
    params.set_default_remaining_outputs();
    return;
  }

  auto fn = std::make_shared<SampleIndexFunction>(std::move(list));
  auto op = FieldOperation::Create(std::move(fn), {std::move(index)});

  params.set_output("Value", GField(std::move(op)));
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeListGetElement");
  ntype.ui_name = "Get Element";
  ntype.ui_description = "Retrieve an element from a list";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.draw_buttons = node_layout;
  ntype.declare = node_declare;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_get_element_cc
