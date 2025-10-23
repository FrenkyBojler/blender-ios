/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_array.hh"
#include "BLI_vector.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_list_unique_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.add_default_layout();

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input(type, "List").structure_type(StructureType::List).hide_value();
    b.add_output(type, "Unique")
        .structure_type(StructureType::List)
        .description("List of unique values from the input");
  }

  b.add_output<decl::Int>("Counts")
      .structure_type(StructureType::List)
      .description("Number of times each unique value appears");
  b.add_output<decl::Int>("Inverse")
      .structure_type(StructureType::List)
      .description("Indices to reconstruct the original list from unique values");
}

static void node_layout(uiLayout *layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout->prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

class SocketSearchOp {
 public:
  const StringRef socket_name;
  eNodeSocketDatatype socket_type;
  void operator()(LinkSearchOpParams &params)
  {
    bNode &node = params.add_node("GeometryNodeListUnique");
    node.custom1 = socket_type;
    params.update_and_connect_available_socket(node, socket_name);
  }
};

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  if (!U.experimental.use_geometry_nodes_lists) {
    return;
  }
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(params.other_socket().type);
  if (params.in_out() == SOCK_IN) {
    params.add_item(IFACE_("List"), SocketSearchOp{"List", socket_type});
  }
  else {
    if (socket_type == SOCK_INT) {
      params.add_item(IFACE_("Counts"), SocketSearchOp{"Counts", SOCK_INT});
      params.add_item(IFACE_("Inverse"), SocketSearchOp{"Inverse", SOCK_INT});
    }
    params.add_item(IFACE_("Unique"), SocketSearchOp{"Unique", socket_type});
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  ListPtr list = params.extract_input<ListPtr>("List");

  if (!list) {
    params.set_default_remaining_outputs();
    return;
  }

  const int list_size = list->size();

  if (list_size == 0) {
    const CPPType &type = list->cpp_type();
    List::ArrayData empty_data = List::ArrayData::ForDefaultValue(type, 0);
    ListPtr empty_list = List::create(type, std::move(empty_data), 0);
    params.set_output("Unique", std::move(empty_list));

    const CPPType &int_type = CPPType::get<int>();
    List::ArrayData empty_int_data = List::ArrayData::ForDefaultValue(int_type, 0);
    params.set_output("Counts", List::create(int_type, std::move(empty_int_data), 0));
    params.set_output("Inverse", List::create(int_type, std::move(empty_int_data), 0));
    return;
  }

  const CPPType &type = list->cpp_type();
  const GVArray input_varray = list->varray();

  Vector<int> unique_indices;
  Vector<int> unique_counts;
  Array<int> inverse_indices(list_size);

  BUFFER_FOR_CPP_TYPE_VALUE(type, element_buffer);

  for (int i = 0; i < list_size; i++) {
    input_varray.get_to_uninitialized(i, element_buffer);

    int unique_index = -1;
    for (int j = 0; j < unique_indices.size(); j++) {
      BUFFER_FOR_CPP_TYPE_VALUE(type, unique_element_buffer);
      input_varray.get_to_uninitialized(unique_indices[j], unique_element_buffer);

      if (type.is_equal(element_buffer, unique_element_buffer)) {
        unique_index = j;
        type.destruct(unique_element_buffer);
        break;
      }
      type.destruct(unique_element_buffer);
    }

    if (unique_index == -1) {
      unique_index = unique_indices.size();
      unique_indices.append(i);
      unique_counts.append(1);
    }
    else {
      unique_counts[unique_index]++;
    }

    inverse_indices[i] = unique_index;
    type.destruct(element_buffer);
  }

  const int unique_count = unique_indices.size();

  List::ArrayData unique_data = List::ArrayData::ForUninitialized(type, unique_count);
  GMutableSpan unique_span(type, unique_data.data, unique_count);

  for (int i = 0; i < unique_count; i++) {
    input_varray.get_to_uninitialized(unique_indices[i], unique_span[i]);
  }

  ListPtr unique_list = List::create(type, std::move(unique_data), unique_count);
  params.set_output("Unique", std::move(unique_list));

  const CPPType &int_type = CPPType::get<int>();
  List::ArrayData counts_data = List::ArrayData::ForUninitialized(int_type, unique_count);
  GMutableSpan counts_span(int_type, counts_data.data, unique_count);

  for (int i = 0; i < unique_count; i++) {
    int_type.copy_construct(&unique_counts[i], counts_span[i]);
  }

  ListPtr counts_list = List::create(int_type, std::move(counts_data), unique_count);
  params.set_output("Counts", std::move(counts_list));

  List::ArrayData inverse_data = List::ArrayData::ForUninitialized(int_type, list_size);
  GMutableSpan inverse_span(int_type, inverse_data.data, list_size);

  for (int i = 0; i < list_size; i++) {
    int_type.copy_construct(&inverse_indices[i], inverse_span[i]);
  }

  ListPtr inverse_list = List::create(int_type, std::move(inverse_data), list_size);
  params.set_output("Inverse", std::move(inverse_list));
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
        return enum_items_filter(
            rna_enum_node_socket_data_type_items, [](const EnumPropertyItem &item) -> bool {
              return socket_type_supports_fields(eNodeSocketDatatype(item.value));
            });
      });
}

static void node_register()
{
  static blender::bke::bNodeType ntype;
  geo_node_type_base(&ntype, "GeometryNodeListUnique");
  ntype.ui_name = "Unique List";
  ntype.ui_description = "Find unique values in a list with counts and inverse mapping";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_unique_cc
