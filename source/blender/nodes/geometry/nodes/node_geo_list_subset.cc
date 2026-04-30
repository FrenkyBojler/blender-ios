/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_node_socket_value.hh"

#include "NOD_geometry_nodes_list.hh"
#include "NOD_rna_define.hh"
#include "NOD_socket.hh"
#include "NOD_socket_search_link.hh"

#include "RNA_enum_types.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "list_function_eval.hh"
#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_list_subset_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  const bNode *node = b.node_or_null();

  b.use_custom_socket_order();
  b.allow_any_socket_order();
  b.add_default_layout();

  if (node != nullptr) {
    const eNodeSocketDatatype type = eNodeSocketDatatype(node->custom1);
    b.add_input(type, "List"_ustr).structure_type(StructureType::List).hide_value();
    b.add_output(type, "List"_ustr).structure_type(StructureType::List).align_with_previous();
  }

  b.add_input<decl::Bool>("Mask"_ustr)
      .default_value(true)
      .hide_value()
      .structure_type(StructureType::Dynamic)
      .description("Boolean mask selecting which values to keep (can be a list or field)");
}

static void node_layout(ui::Layout &layout, bContext * /*C*/, PointerRNA *ptr)
{
  layout.prop(ptr, "data_type", UI_ITEM_NONE, "", ICON_NONE);
}

class SocketSearchOp {
 public:
  UString socket_name;
  eNodeSocketDatatype socket_type;
  void operator()(LinkSearchOpParams &params)
  {
    bNode &node = params.add_node("GeometryNodeListSubset"_ustr);
    node.custom1 = socket_type;
    params.update_and_connect_available_socket(node, socket_name);
  }
};

static void node_gather_link_searches(GatherLinkSearchOpParams &params)
{
  const eNodeSocketDatatype socket_type = eNodeSocketDatatype(params.other_socket().type);
  if (params.in_out() == SOCK_IN) {
    if (socket_type == SOCK_BOOLEAN) {
      params.add_item(IFACE_("Mask"), [](LinkSearchOpParams &params) {
        bNode &node = params.add_node("GeometryNodeListSubset"_ustr);
        params.update_and_connect_available_socket(node, "Mask"_ustr);
      });
    }
    else {
      params.add_item(IFACE_("List"), SocketSearchOp{"List"_ustr, socket_type});
    }
  }
  else {
    params.add_item(IFACE_("List"), SocketSearchOp{"List"_ustr, socket_type});
  }
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GListPtr list = params.extract_input<GListPtr>("List"_ustr);

  if (!list) {
    params.set_default_remaining_outputs();
    return;
  }

  if (!params.output_is_required("List"_ustr)) {
    return;
  }

  const int list_size = list->size();

  if (list_size == 0) {
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  bke::SocketValueVariant mask_variant = params.extract_input<bke::SocketValueVariant>(
      "Mask"_ustr);

  GListPtr mask_list;
  if (mask_variant.is_context_dependent_field()) {
    fn::GField field = mask_variant.extract<fn::GField>();
    mask_list = evaluate_field_to_list(std::move(field), list_size);
    if (!mask_list) {
      params.error_message_add(NodeWarningType::Error, "Failed to evaluate mask field");
      params.set_output("List"_ustr, std::move(list));
      return;
    }
  }
  else if (mask_variant.is_list()) {
    mask_list = mask_variant.get<GListPtr>();
    if (!mask_list) {
      params.set_output("List"_ustr, std::move(list));
      return;
    }
    if (mask_list->size() != list_size) {
      params.error_message_add(
          NodeWarningType::Error,
          "List and Mask must have the same length (List: " + std::to_string(list_size) +
              ", Mask: " + std::to_string(mask_list->size()) + ")");
      params.set_default_remaining_outputs();
      return;
    }
  }
  else if (mask_variant.is_single()) {
    /* A single true keeps everything, a single false keeps nothing. */
    if (mask_variant.get<bool>()) {
      params.set_output("List"_ustr, std::move(list));
    }
    else {
      const CPPType &type = list->cpp_type();
      GList::ArrayData empty_data = GList::ArrayData::ForDefaultValue(type, 0);
      params.set_output("List"_ustr, GList::create(type, std::move(empty_data), 0));
    }
    return;
  }
  else {
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  const VArray<bool> mask_varray = mask_list->varray().typed<bool>();

  /* Count how many values pass the mask. */
  int result_size = 0;
  for (int i = 0; i < list_size; i++) {
    if (mask_varray[i]) {
      result_size++;
    }
  }

  if (result_size == 0) {
    const CPPType &type = list->cpp_type();
    GList::ArrayData empty_data = GList::ArrayData::ForDefaultValue(type, 0);
    params.set_output("List"_ustr, GList::create(type, std::move(empty_data), 0));
    return;
  }

  if (result_size == list_size) {
    params.set_output("List"_ustr, std::move(list));
    return;
  }

  const CPPType &type = list->cpp_type();
  const GVArray src_varray = list->varray();

  GList::ArrayData result_data = GList::ArrayData::ForUninitialized(type, result_size);
  GMutableSpan dst_span = result_data.span_for_write(type, result_size);

  int dst_index = 0;
  for (int i = 0; i < list_size; i++) {
    if (mask_varray[i]) {
      src_varray.get_to_uninitialized(i, dst_span[dst_index]);
      dst_index++;
    }
  }

  GListPtr result_list = GList::create(type, std::move(result_data), result_size);
  params.set_output("List"_ustr, std::move(result_list));
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
  geo_node_type_base(&ntype, "GeometryNodeListSubset"_ustr);
  ntype.ui_name = "Subset List";
  ntype.ui_description = "Filter a list using a boolean mask, keeping only the true values";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.geometry_node_execute = node_geo_exec;
  ntype.declare = node_declare;
  ntype.draw_buttons = node_layout;
  ntype.gather_link_search_ops = node_gather_link_searches;
  blender::bke::node_register_type(ntype);
  node_rna(ntype.rna_ext.srna);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_list_subset_cc
